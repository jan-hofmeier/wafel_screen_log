/* wafel_screen_log - print the IOSU log (and crashes) on the TV.
 *
 * kern_main (MMU off): take over the TV if minute left it running, otherwise
 * init it like minute does; start drawing.
 * Log sources:
 *   - wafel_core debug_printf + semihosting write0 (call sites of
 *     serial_send_str): core and plugin messages, 5debug's syslog mirror
 *   - MCP ringbuf_write_overwrite (0x05055324): the whole IOSU syslog
 * Drawing:
 *   - kern_main phase: incremental, straight to the physical framebuffer
 *   - from mcp_main on: an MCP thread with a scrolling view
 *   - crashes: crash.c (MMU off)
 * The framebuffer (0x17500000) is PPC memory: we never touch it again once
 * MCP starts the PPC (except for the crash screen, when everything halts).
 * Build variants: default = stop at PPC start (the last picture stays until
 * Cafe OS reprograms the TV), SCRLOG_OVERLAY = once Cafe OS scans out its own
 * surface, keep drawing the last lines over it. */
#include "scrlog.h"
#include "hw.h"
#include <wafel/dynamic.h>
#include <wafel/ios_dynamic.h>
#include <wafel/trampoline.h>
#include <wafel/utils.h>
#include <wafel/ios/svc.h>
#include <wafel/ios/thread.h>
#include <wafel/ios/prsh.h>
#include <wafel/latte/cache.h>
extern const unsigned char scrlog_font[];
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#define PON_SMC_TIMER    0x00020000   /* minute rtc.h: ECO / timer wake */
#define PFLAG_PON_RELOAD 0x00002000   /* minute rtc.h: IOSU reload */

/* MCP 5.5.x */
#define MCP_SYSLOG_RINGBUF_BL  0x05055334  /* bl ringbuf_write (r1 = data, r2 = len) */
#define MCP_SYSLOG_RINGBUF_FN  0x05055294
#define MCP_PPC_START_BL       0x050340ee  /* loadCafeOSKernel: bl MCP_bspWrite (PPC start), thumb */
#define MCP_BSPWRITE           0x05059570

#define RENDER_PERIOD_MS       50
#define OVERLAY_TIMEOUT_TICKS  (10000 / RENDER_PERIOD_MS)
#define ALIVE_CHECK_TICKS      (1000 / RENDER_PERIOD_MS)

volatile int display_on;
volatile int mcp_renderer;
static volatile int ppc_started;
/* Set by another plugin (found with wafel_find_symbol) that draws its own
 * screen before the PPC starts, e.g. wafel_recovery_setup just before MCP's
 * Boot Recovery menu: from then on only the last lines are drawn, in a box at
 * the bottom, and the TV is never re-initialised. */
volatile int scrlog_bottom_only;
static volatile int render_busy;
static int ppc_hooked;
static int eco_boot;
static int took_over_minute;
static dc_state_t our_dc;             /* scanout state we draw into */

surface_t tv_phys = {
    .fb = (u32 *)FB_TV_ADDR, .pitch = FB_TV_W, .w = FB_TV_W, .h = FB_TV_H, .flush = 0,
};
static surface_t tv_mcp;

static void surface_from_dc(surface_t *s, const dc_state_t *d, int flush)
{
    s->fb = (u32 *)(d->surface & ~0xFFu);
    s->pitch = d->pitch;
    s->w = d->xend;
    s->h = d->yend;
    s->flush = flush;
}

/* ---- log hooks ------------------------------------------------------------ */

/* call site of serial_send_str in debug_printf / semihosting_handler; r0 = str */
static bool serial_hook(trampoline_state *st)
{
    log_puts((const char *)st->r[0]);
    if (display_on && !mcp_renderer && !ppc_started && !crash_active && cpu_is_physical())
        render_direct(&tv_phys);
    return false;   /* run the original call */
}

/* Replacement for wafel_core's debug_printf (core's entry is patched with a
 * B to this, so lr stays the caller's). Some core builds only print when a
 * plugin enables debug output (5debug: "Enabling debug_printf"); without it
 * core's debug_printf returns before reaching serial_send_str, so hooking that
 * call is not enough. We always log; serial only if core would have printed. */
static int (*core_serial_send_str)(const char *);
static int core_serial_enabled;
static char dbg_buf[0x200];

void scrlog_debug_printf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    vsnprintf(dbg_buf, sizeof(dbg_buf), fmt, va);
    va_end(va);

    log_puts(dbg_buf);
    if (display_on && !mcp_renderer && !ppc_started && !crash_active && cpu_is_physical())
        render_direct(&tv_phys);
    if (core_serial_enabled && core_serial_send_str)
        core_serial_send_str(dbg_buf);
}

static int take_over_debug_printf(uintptr_t sss)
{
    uintptr_t dp = wafel_find_symbol("debug_printf");
    if (!dp || !sss)
        return 0;
    core_serial_send_str = (int (*)(const char *))sss;
    /* same rule as core: some plugin turns debug output on */
    core_serial_enabled = wafel_find_symbol("enable_debug_printf") != 0
                       || wafel_find_symbol("sys_write_r1") != 0;
    s32 off = ((s32)(uintptr_t)scrlog_debug_printf - (s32)dp - 8) / 4;
    *(volatile u32 *)dp = 0xEA000000 | ((u32)off & 0xFFFFFF);
    return 1;
}

/* MCP: ringbuf_write_overwrite -> bl ringbuf_write(ring, data, len) */
static void syslog_hook(trampoline_state *st)
{
    u32 len = (u32)st->r[2];
    if (len > 0x2000)        /* Syslog_Thread's own limit */
        len = 0x2000;
    log_append((const char *)st->r[1], len);
}

/* MCP thread, right before MCP tells BSP to start the PPC: from now on the
 * framebuffer memory belongs to Cafe OS. Wait for a running redraw. */
static void ppc_start_hook(trampoline_t_state *st)
{
    (void)st;
    ppc_started = 1;
    for (int i = 0; i < 100 && render_busy; i++)
        msleep(2);
}

/* ---- hook installation helpers ------------------------------------------------ */

static u32 arm_branch_target(uintptr_t at, u32 ins)
{
    s32 off = (s32)(ins << 8) >> 6;     /* sign-extended imm24 * 4 */
    return at + 8 + off;
}

/* Hook every unconditional B/BL to 'callee' inside fn (scans until bx lr). */
static int hook_calls_to(const char *fn_name, uintptr_t callee, bool (*h)(trampoline_state *))
{
    uintptr_t fn = wafel_find_symbol(fn_name);
    int n = 0;
    if (!fn)
        return 0;
    for (int i = 0; i < 64; i++) {
        uintptr_t at = fn + 4 * i;
        u32 ins = *(u32 *)at;
        if ((ins >> 24) == 0xEA || (ins >> 24) == 0xEB) {
            if (arm_branch_target(at, ins) == callee) {
                trampoline_hook_before_v2(at, h);
                n++;
            }
            if ((ins >> 24) == 0xEA)
                break;              /* tail call: end of function */
        }
        if (ins == 0xE12FFF1E)      /* bx lr */
            break;
    }
    return n;
}

static int arm_bl_at(uintptr_t vaddr, u32 target)
{
    u32 ins = *(u32 *)ios_elf_vaddr_to_paddr(vaddr);
    return (ins >> 24) == 0xEB && arm_branch_target(vaddr, ins) == target;
}

static int thumb_bl_at(uintptr_t vaddr, u32 target)
{
    u16 *p = (u16 *)ios_elf_vaddr_to_paddr(vaddr);
    u16 h1 = p[0], h2 = p[1];
    if ((h1 >> 11) != 0x1E || (h2 >> 11) != 0x1F)
        return 0;
    s32 off = ((s32)((u32)h1 << 21) >> 9) | ((h2 & 0x7FF) << 1);
    return (u32)(vaddr + 4 + off) == target;
}

/* ---- display setup ------------------------------------------------------------ */

static void display_setup(void)
{
    dc_state_t d;
    display_read_state(&d);
#ifndef SCRLOG_FORCE_INIT
    if (dc_state_usable(&d)) {
        took_over_minute = 1;           /* minute (or Cafe OS before a reload) left it running */
    } else
#endif
    {
        display_init();
        display_read_state(&d);
    }
    our_dc = d;
    surface_from_dc(&tv_phys, &d, 0);
}

#ifdef SCRLOG_DIAG
/* border, 1px grid every 64px in the top-right corner, 8x8 checker: shows
 * pitch/tiling/scaling problems at a glance */
static void draw_test_pattern(surface_t *s)
{
    for (u32 x = 0; x < s->w; x++) {
        s->fb[x] = COL_FG;
        s->fb[(s->h - 1) * s->pitch + x] = COL_FG;
    }
    for (u32 y = 0; y < s->h; y++) {
        s->fb[y * s->pitch] = COL_FG;
        s->fb[y * s->pitch + s->w - 1] = COL_FG;
    }
    for (u32 y = 0; y < 128; y++)
        for (u32 x = s->w - 256; x < s->w - 128; x++)
            s->fb[y * s->pitch + x] = (((x >> 3) ^ (y >> 3)) & 1) ? COL_FG : COL_BG;
    for (u32 y = 0; y < 128; y++)
        for (u32 x = s->w - 128; x < s->w; x++)
            if ((x % 16) == 0 || (y % 16) == 0)
                s->fb[y * s->pitch + x] = COL_FG;
}
#endif

/* ---- MCP render thread ------------------------------------------------------- */

static u32 render_thread(void *arg)
{
    (void)arg;
    u32 tick = 0, ppc_ticks = 0, last_wpos = (u32)-1;
    int overlay_active = 0;

    tv_mcp = tv_phys;
    tv_mcp.flush = 1;
    u32 size = tv_mcp.pitch * tv_mcp.h * 4;
    if (iosCheckIosAddrRange(tv_mcp.fb, size, MEM_PERM_RW) < 0
        || iosVirtToPhys(tv_mcp.fb) != (u32)tv_phys.fb) {
        debug_printf("scrlog: framebuffer %08X not mapped 1:1 in MCP (phys %08X), MCP drawing disabled\n",
                     (u32)tv_mcp.fb, iosVirtToPhys(tv_mcp.fb));
        return 0;
    }

    render_busy = 1;
    if (!ppc_started) {
        mcp_renderer = 1;
        render_invalidate();
        surface_clear(&tv_mcp);
    }
    render_busy = 0;

    debug_printf("scrlog: render thread running, fb %08X %ux%u pitch %u\n",
                 (u32)tv_mcp.fb, tv_mcp.w, tv_mcp.h, tv_mcp.pitch);

    while (1) {
        msleep(RENDER_PERIOD_MS);
        tick++;

        if (crash_active)
            continue;

        if (!ppc_started) {
            render_busy = 1;
            if (ppc_started) {          /* raced with the PPC start hook */
                render_busy = 0;
                continue;
            }
            if (scrlog_bottom_only) {
                if (log_wpos != last_wpos) {
                    last_wpos = log_wpos;
                    render_overlay(&tv_mcp);
                }
                render_busy = 0;
                continue;
            }
            /* Something may have reset the display. Only while we can see
             * the PPC start; otherwise we could fight Cafe OS for the TV. */
            if (ppc_hooked && tick % ALIVE_CHECK_TICKS == 0) {
                dc_state_t d;
                display_read_state(&d);
                if (!dc_state_same(&d, &our_dc)) {
                    display_init();
                    display_read_state(&our_dc);
                    surface_from_dc(&tv_phys, &our_dc, 0);
                    tv_mcp = tv_phys;
                    tv_mcp.flush = 1;
                    surface_clear(&tv_mcp);
                    render_invalidate();
                    debug_printf("scrlog: TV output changed (surf %08X ctl %08X), re-initialised it\n",
                                 d.surface, d.control);
                }
            }
            if (log_wpos != last_wpos) {
                last_wpos = log_wpos;
                render_full(&tv_mcp);
            }
            render_busy = 0;
            continue;
        }

        /* PPC started: our framebuffer is Cafe OS's memory now */
        if (mcp_renderer) {
            mcp_renderer = 0;
            debug_printf("scrlog: PPC started, no more drawing to %08X\n", (u32)tv_phys.fb);
        }
#ifndef SCRLOG_OVERLAY
        break;
#else
        if (!overlay_active) {
            dc_state_t d;
            display_read_state(&d);
            ppc_ticks++;
            /* wait until Cafe OS scans out its own surface (or 10 s: then
             * whatever is scanned out is Cafe OS's picture anyway) */
            if (dc_state_same(&d, &our_dc) && ppc_ticks < OVERLAY_TIMEOUT_TICKS)
                continue;
            overlay_active = 1;
            debug_printf("scrlog: Cafe OS scans out %08X ctl %08X %ux%u, overlay on\n",
                         d.surface, d.control, d.xend, d.yend);
        }
        {
            dc_state_t d;
            surface_t s;
            display_read_state(&d);
            if (!dc_state_usable(&d))
                continue;
            surface_from_dc(&s, &d, 1);
            if (iosCheckIosAddrRange(s.fb, s.pitch * s.h * 4, MEM_PERM_RW) < 0
                || iosVirtToPhys(s.fb) != (u32)s.fb)
                continue;
            render_overlay(&s);
        }
#endif
    }

    while (1)
        msleep(1000);
    return 0;
}

/* ---- entry points ---------------------------------------------------------- */

__attribute__((target("arm")))
void kern_main(void)
{
    boot_info_t *bi = NULL;
    u32 boot_state = 0;
    if (prsh_get_entry("boot_info", (void **)&bi, NULL) == 0 && bi)
        boot_state = bi->boot_state;
    eco_boot = (boot_state & PON_SMC_TIMER) != 0;

    dc_state_t pre;
    display_read_state(&pre);
    u32 pre898 = abif_gpu_read32(0x898);

    /* 1. screen first, so failures in the hook installs below are visible */
    if (!eco_boot) {
        display_setup();
        display_on = 1;
        if (cpu_is_physical()) {
            surface_clear(&tv_phys);
#ifdef SCRLOG_DIAG
            draw_test_pattern(&tv_phys);
#endif
        }
    }

    /* 2. the printf path: from here on core/plugin output is drawn */
    /* With 5debug installed, every syslog chunk also arrives as semihosting
     * write0; we take the syslog from MCP's ring directly, so skip that path. */
    int have_5debug = wafel_find_symbol("sys_write_r1") != 0;
    uintptr_t sss = wafel_find_symbol("serial_send_str");
    int n_printf = 0, n_semi = 0;
    if (sss) {
        n_printf = take_over_debug_printf(sss);
        if (!have_5debug)
            n_semi = hook_calls_to("semihosting_handler", sss, serial_hook);
    }
    ic_invalidateall();

    debug_printf("wafel_screen_log " SCRLOG_VERSION
#ifdef SCRLOG_OVERLAY
                 " (overlay)"
#else
                 " (stops at PPC start)"
#endif
                 ": boot_state %08X%s%s, TV %s\n", boot_state,
                 (boot_state & PFLAG_PON_RELOAD) ? " IOSU-reload" : "",
                 eco_boot ? " ECO" : "",
                 eco_boot ? "left off" : (took_over_minute ? "taken over" : "initialised"));

    /* 3. the rest */
    /* The whole IOSU syslog. 5debug hooks the same instruction later; its
     * trampoline_hook_before sees our BL there and chains to it, so both run. */
    int syslog_hooked = 0;
    if (arm_bl_at(MCP_SYSLOG_RINGBUF_BL, MCP_SYSLOG_RINGBUF_FN)) {
        trampoline_hook_before(MCP_SYSLOG_RINGBUF_BL, syslog_hook);
        syslog_hooked = 1;
    }
    if (thumb_bl_at(MCP_PPC_START_BL, MCP_BSPWRITE)) {
        trampoline_t_hook_before(MCP_PPC_START_BL, ppc_start_hook);
        ppc_hooked = 1;
    }
    crash_install_hooks();
    ic_invalidateall();

    debug_printf("scrlog: hooks: printf %s (serial %s), semihosting %d%s, syslog %s, ppc %s\n",
                 n_printf ? "taken over" : "NO", core_serial_enabled ? "on" : "off", n_semi, have_5debug ? " (skipped, 5debug)" : "",
                 syslog_hooked ? "yes" : "NO", ppc_hooked ? "yes" : "NO");
#ifdef SCRLOG_DIAG
    debug_printf("scrlog: before: surf %08X pitch %X ctl %08X %ux%u ht/vt %u/%u en %X crtc %08X swap %X 898 %08X\n",
                 pre.surface, pre.pitch, pre.control, pre.xend, pre.yend, pre.htotal, pre.vtotal,
                 pre.grph_en, pre.crtc, pre.swap, pre898);
    dc_state_t post;
    display_read_state(&post);
    debug_printf("scrlog: after:  surf %08X pitch %X ctl %08X %ux%u ht/vt %u/%u en %X crtc %08X swap %X 898 %08X\n",
                 post.surface, post.pitch, post.control, post.xend, post.yend, post.htotal, post.vtotal,
                 post.grph_en, post.crtc, post.swap, abif_gpu_read32(0x898));
    debug_printf("scrlog: cp15 ctrl %08X cpsr %08X font %08X\n",
                 cpu_cp15_ctrl_read(), cpu_get_cpsr(), (u32)scrlog_font);
    debug_printf("scrlog: font test: ABCabc 0123 !?#\n");
#else
    (void)pre;
    (void)pre898;
#endif
}

void mcp_main(void)
{
    if (!display_on)
        return;

    u8 *stack = (u8 *)iosAllocAligned(0x0001, 0x2000, 0x20);
    if (!stack) {
        debug_printf("scrlog: no stack for the render thread\n");
        return;
    }
    int th = iosCreateThread(render_thread, NULL, (u32 *)(stack + 0x2000), 0x2000, 0x20, 1);
    if (th < 0) {
        debug_printf("scrlog: create render thread failed %d\n", th);
        return;
    }
    iosStartThread(th);
}
