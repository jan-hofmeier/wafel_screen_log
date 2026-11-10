/* wafel_screen_log - crash screen for aborts, undefined instructions and
 * IOS panics.
 *
 * wafel_core points the kernel's abort/undef vectors and the IOS panic
 * function at iabt_replace / dabt_replace / undef_abt_replace /
 * ios_panic_replace (wafel_core/source/main.c). Those print to serial and
 * spin forever. We hook their first instruction (they never return, so the
 * lr clobbered by the hook's BL doesn't matter), draw a compact crash screen
 * and return, so wafel_core still prints its full dump to serial.
 *
 * The faulting context's page tables may not map the framebuffer, so the
 * screen is drawn with the MMU and D-cache switched off on our own stack.
 * That relies on the plugin carveout being identity mapped (it is: plugin
 * kern_main runs with the MMU off at the same addresses MCP uses later).
 * Everything that lives at a virtual address of the faulting context (panic
 * string, exception save area) is copied before the MMU goes off. */
#include "scrlog.h"
#include "hw.h"
#include <wafel/dynamic.h>
#include <wafel/trampoline.h>
#include <wafel/utils.h>
#include <string.h>
#include <stdio.h>

volatile int crash_active;

enum { CRASH_IABT, CRASH_DABT, CRASH_UNDEF, CRASH_PANIC };

#define CRASH_STACK_WORDS 48

typedef struct {
    int type;
    int have_regs;
    u32 regs[17];           /* save area words 0..16 as wafel_core prints them */
    int have_insn;
    u32 insn;
    u32 stack[CRASH_STACK_WORDS];
    char msg[160];
    u32 cpsr;
} crash_info_t;

static crash_info_t crash_info;
static u8 crash_stack[0x2000] __attribute__((aligned(8)));
static char crash_cells[TV_ROWS * TV_COLS];

static const char *crash_title(int type)
{
    switch (type) {
    case CRASH_IABT:  return "PREFETCH ABORT";
    case CRASH_DABT:  return "DATA ABORT";
    case CRASH_UNDEF: return "UNDEFINED INSTRUCTION";
    default:          return "IOS PANIC";
    }
}

static void line(surface_t *s, u32 *row, const char *t, u32 fg, u32 bg)
{
    if (*row >= TV_ROWS)
        return;
    u32 n = strlen(t);
    if (n > TV_COLS)
        n = TV_COLS;
    char pad[TV_COLS];
    memset(pad, ' ', sizeof(pad));
    memcpy(pad, t, n);
    draw_text(s, MARGIN_X, MARGIN_Y + *row * LINE_H, pad, TV_COLS, fg, bg);
    (*row)++;
}

/* runs with MMU + D-cache off, on crash_stack */
static void crash_draw(void *arg)
{
    crash_info_t *ci = arg;
    static surface_t crash_surface;
    surface_t *s = &crash_surface;
    char buf[TV_COLS + 1];
    u32 row = 0;

    /* Whoever has the TV (minute, us, Cafe OS): if it scans out a linear
     * 32bpp surface, draw into that; otherwise init our own mode. The
     * system is halting, so writing PPC memory doesn't matter any more. */
    dc_state_t d;
    display_read_state(&d);
    if (!dc_state_usable(&d)) {
        display_init();
        display_read_state(&d);
    }
    s->fb = (u32 *)(d.surface & ~0xFFu);
    s->pitch = d.pitch;
    s->w = d.xend > FB_TV_W ? FB_TV_W : d.xend;   /* layout is for 1280x720 */
    s->h = d.yend > FB_TV_H ? FB_TV_H : d.yend;
    s->flush = 0;
    display_on = 1;

    surface_clear(s);

    snprintf(buf, sizeof(buf), " *** %s ***   (wafel_screen_log " SCRLOG_VERSION ", cpsr %08lX)",
             crash_title(ci->type), (unsigned long)ci->cpsr);
    line(s, &row, buf, COL_BG, COL_FG);      /* inverted header */
    row++;

    if (ci->msg[0]) {
        snprintf(buf, sizeof(buf), "panic: %s", ci->msg);
        line(s, &row, buf, COL_FG, COL_BG);
        row++;
    }

    if (ci->have_regs) {
        /* wafel_core's abt_replace: rN = save[N+1], pc = save[16] */
        for (u32 i = 0; i < 16; i += 4) {
            snprintf(buf, sizeof(buf), "r%-2lu %08lX   r%-2lu %08lX   r%-2lu %08lX   r%-2lu %08lX",
                     (unsigned long)i,     (unsigned long)ci->regs[i + 1],
                     (unsigned long)i + 1, (unsigned long)ci->regs[i + 2],
                     (unsigned long)i + 2, (unsigned long)ci->regs[i + 3],
                     (unsigned long)i + 3, (unsigned long)ci->regs[i + 4]);
            line(s, &row, buf, COL_FG, COL_BG);
        }
        snprintf(buf, sizeof(buf), "save[0] %08lX   (r13=sp r14=lr r15=pc of the faulting context)",
                 (unsigned long)ci->regs[0]);
        line(s, &row, buf, COL_FG, COL_BG);
        if (ci->have_insn)
            snprintf(buf, sizeof(buf), "faulting instruction: %08lX", (unsigned long)ci->insn);
        else if (ci->type == CRASH_IABT)
            snprintf(buf, sizeof(buf), "fetch failed at %08lX (r15 - 4); instruction not readable",
                     (unsigned long)(ci->regs[16] - 4));
        else
            snprintf(buf, sizeof(buf), "faulting instruction: (not readable)");
        line(s, &row, buf, COL_FG, COL_BG);
        row++;

        line(s, &row, "exception stack (save area +0x44..):", COL_FG, COL_BG);
        for (u32 i = 0; i < CRASH_STACK_WORDS; i += 8) {
            snprintf(buf, sizeof(buf), "%08lX %08lX %08lX %08lX  %08lX %08lX %08lX %08lX",
                     (unsigned long)ci->stack[i],     (unsigned long)ci->stack[i + 1],
                     (unsigned long)ci->stack[i + 2], (unsigned long)ci->stack[i + 3],
                     (unsigned long)ci->stack[i + 4], (unsigned long)ci->stack[i + 5],
                     (unsigned long)ci->stack[i + 6], (unsigned long)ci->stack[i + 7]);
            line(s, &row, buf, COL_FG, COL_BG);
        }
        row++;
    }

    line(s, &row, "--- last log lines ---", COL_BG, COL_FG);
    if (row < TV_ROWS) {
        u32 rows = TV_ROWS - row;
        render_layout(crash_cells, rows, TV_COLS);
        for (u32 r = 0; r < rows; r++, row++)
            draw_text(s, MARGIN_X, MARGIN_Y + row * LINE_H, &crash_cells[r * TV_COLS],
                      TV_COLS, COL_FG, COL_BG);
    }
}

static void crash_show(void)
{
    if (!cpu_is_privileged())
        return;     /* never expected: aborts and panics run privileged */

    u32 cpsr = cpu_irq_disable();
    u32 ctrl = cpu_cp15_ctrl_read();
    int mmu = ctrl & CP15_MMU;
    crash_info.cpsr = cpsr;

    if (mmu) {
        cpu_dcache_clean_all();
        cpu_cp15_ctrl_write(ctrl & ~(CP15_MMU | CP15_DCACHE));
    }

    cpu_call_on_stack(crash_draw, &crash_info, crash_stack + sizeof(crash_stack));

    if (mmu) {
        cpu_dcache_invalidate_all();
        cpu_cp15_ctrl_write(ctrl);
    }
    cpu_irq_restore(cpsr);
}

static void crash_from_abort(int type, u32 *save)
{
    if (crash_active)
        return;
    crash_active = 1;

    memset(&crash_info, 0, sizeof(crash_info));
    crash_info.type = type;
    if (save) {
        crash_info.have_regs = 1;
        for (int i = 0; i < 17; i++)
            crash_info.regs[i] = save[i];
        for (int i = 0; i < CRASH_STACK_WORDS; i++)
            crash_info.stack[i] = save[17 + i];
        /* same offsets as wafel_core: dabt pc-8, undef pc-4. A prefetch abort
         * pc is by definition not readable, don't touch it. */
        if (type == CRASH_DABT || type == CRASH_UNDEF) {
            u32 pc = save[16] - (type == CRASH_DABT ? 8 : 4);
            if ((pc & 3) == 0 && pc != 0) {
                crash_info.insn = *(volatile u32 *)pc;
                crash_info.have_insn = 1;
            }
        }
    }
    crash_show();
}

static void hook_iabt(trampoline_state *st)  { crash_from_abort(CRASH_IABT,  (u32 *)st->r[0]); }
static void hook_dabt(trampoline_state *st)  { crash_from_abort(CRASH_DABT,  (u32 *)st->r[0]); }
static void hook_undef(trampoline_state *st) { crash_from_abort(CRASH_UNDEF, (u32 *)st->r[0]); }

static void hook_panic(trampoline_state *st)
{
    if (crash_active)
        return;
    crash_active = 1;
    memset(&crash_info, 0, sizeof(crash_info));
    crash_info.type = CRASH_PANIC;
    const char *msg = (const char *)st->r[0];
    if (msg) {
        u32 i;
        for (i = 0; i < sizeof(crash_info.msg) - 1 && msg[i]; i++)
            crash_info.msg[i] = (msg[i] == '\n' || msg[i] == '\r') ? ' ' : msg[i];
        crash_info.msg[i] = 0;
    }
    crash_show();
}

/* The hook stub relocates the first instruction; refuse anything that
 * uses or writes pc, and branches (a BL would be chained, but these
 * handlers don't start with one). */
static int first_insn_relocatable(u32 ins)
{
    if (((ins >> 25) & 7) == 5)          /* B/BL */
        return 0;
    if (((ins >> 16) & 0xF) == 0xF)      /* Rn == pc */
        return 0;
    if (((ins >> 12) & 0xF) == 0xF)      /* Rd == pc (or r12-r15 in a reglist) */
        return 0;
    if ((ins & 0xF) == 0xF && ((ins >> 25) & 7) == 0)  /* Rm == pc */
        return 0;
    return 1;
}

static void hook_core_fn(const char *name, void (*fn)(trampoline_state *))
{
    uintptr_t addr = wafel_find_symbol(name);
    if (!addr) {
        debug_printf("scrlog: %s not found, no crash screen for it\n", name);
        return;
    }
    u32 ins = *(u32 *)addr;
    if (!first_insn_relocatable(ins)) {
        debug_printf("scrlog: %s starts with %08X, not hooking\n", name, ins);
        return;
    }
    trampoline_hook_before(addr, fn);
}

void crash_install_hooks(void)
{
    hook_core_fn("iabt_replace", hook_iabt);
    hook_core_fn("dabt_replace", hook_dabt);
    hook_core_fn("undef_abt_replace", hook_undef);
    hook_core_fn("ios_panic_replace", hook_panic);
}
