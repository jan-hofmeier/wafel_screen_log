#ifndef SCRLOG_H
#define SCRLOG_H

#include <wafel/types.h>

#define SCRLOG_VERSION "0.5"

/* ---- cpu.S ---------------------------------------------------------------- */
u32  cpu_get_cpsr(void);
u32  cpu_cp15_ctrl_read(void);
void cpu_cp15_ctrl_write(u32 v);
void cpu_dcache_clean_all(void);
void cpu_dcache_invalidate_all(void);
void cpu_dcache_clean_range(void *p, u32 size);
u32  cpu_irq_disable(void);
void cpu_irq_restore(u32 old_cpsr);
void cpu_call_on_stack(void (*fn)(void *), void *arg, void *stack_top);

#define CPSR_MODE_USR 0x10
#define CP15_MMU      (1u << 0)
#define CP15_DCACHE   (1u << 2)

static inline int cpu_is_privileged(void)
{
    return (cpu_get_cpsr() & 0x1F) != CPSR_MODE_USR;
}

/* privileged and running without the MMU (plugin kern_main phase) */
static inline int cpu_is_physical(void)
{
    return cpu_is_privileged() && !(cpu_cp15_ctrl_read() & CP15_MMU);
}

/* ---- log.c: text ring ------------------------------------------------------ */
#define LOG_RING_SIZE 0x10000u            /* power of two */
#define LOG_RING_MASK (LOG_RING_SIZE - 1)
extern char log_ring[LOG_RING_SIZE];
extern volatile u32 log_wpos;             /* total bytes ever written */

/* Append text (len == (u32)-1: NUL terminated). Any context, never blocks.
 * Concurrent writers can interleave characters but never corrupt memory. */
void log_append(const char *s, u32 len);
void log_puts(const char *s);

/* ---- render.c -------------------------------------------------------------- */
typedef struct {
    u32 *fb;        /* pointer usable in the current context */
    u32 pitch;      /* in pixels */
    u32 w, h;
    int flush;      /* 0: none (MMU off), 1: iosFlushDCache, 2: cp15 clean */
} surface_t;

#define FONT_W   8
#define FONT_H   8
#define LINE_H   9
#define MARGIN_X 8
#define MARGIN_Y 8
#define TV_COLS  ((1280 - 2 * MARGIN_X) / FONT_W)   /* 158 */
#define TV_ROWS  ((720 - 2 * MARGIN_Y) / LINE_H)    /* 78 */
#define OVL_ROWS 20

#define COL_FG 0xFFFFFFFFu
#define COL_BG 0x00000000u

void surface_clear(surface_t *s);
void surface_flush(surface_t *s, u32 y0, u32 y1);
void draw_text(surface_t *s, u32 x, u32 y, const char *t, u32 n, u32 fg, u32 bg);

/* scrolling full-screen view with a shadow buffer (only changed cells drawn) */
void render_full(surface_t *s);
void render_invalidate(void);
/* cheap incremental wrap-around drawing for the MMU-off kern_main phase */
void render_direct(surface_t *s);
/* opaque box with the last OVL_ROWS lines at the bottom of any surface */
void render_overlay(surface_t *s);
/* lay out the last 'rows' display lines of the ring into cells[rows*cols] */
u32  render_layout(char *cells, u32 rows, u32 cols);

/* ---- crash.c --------------------------------------------------------------- */
void crash_install_hooks(void);
extern volatile int crash_active;

/* ---- main.c state ---------------------------------------------------------- */
extern volatile int display_on;         /* we own/initialised the TV output */
extern volatile int mcp_renderer;       /* MCP render thread is drawing */
extern surface_t tv_phys;               /* FB_TV_ADDR as physical pointer */

#endif
