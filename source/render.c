/* wafel_screen_log - text rendering with wafel_core's 8x8 font.
 *
 * Colours are only white/black (0xFFFFFFFF / 0): those are the same under
 * every byte swap and channel order, so the text is correct on our own
 * surface and on Cafe OS's surfaces alike. */
#include "scrlog.h"
extern const unsigned char scrlog_font[];
#include <wafel/ios/svc.h>
#include <string.h>

#define FONT_FIRST 32
#define FONT_LAST  125      /* font_bin holds 94 glyphs (752 bytes) */

static void draw_char(surface_t *s, u32 x, u32 y, char c, u32 fg, u32 bg)
{
    if (x + FONT_W > s->w || y + LINE_H > s->h)
        return;
    u8 uc = (u8)c;
    if (uc < FONT_FIRST)
        uc = ' ';
    if (uc > FONT_LAST)
        uc = '?';
    const u8 *g = &scrlog_font[(uc - FONT_FIRST) * 8];
    u32 *row = s->fb + y * s->pitch + x;
    for (u32 j = 0; j < FONT_H; j++) {
        u8 bits = g[j];
        for (u32 i = 0; i < FONT_W; i++)
            row[i] = (bits & (1u << i)) ? fg : bg;   /* LSB = leftmost pixel */
        row += s->pitch;
    }
    for (u32 j = FONT_H; j < LINE_H; j++) {
        for (u32 i = 0; i < FONT_W; i++)
            row[i] = bg;
        row += s->pitch;
    }
}

void draw_text(surface_t *s, u32 x, u32 y, const char *t, u32 n, u32 fg, u32 bg)
{
    for (u32 i = 0; i < n && t[i]; i++, x += FONT_W)
        draw_char(s, x, y, t[i], fg, bg);
}

void surface_flush(surface_t *s, u32 y0, u32 y1)
{
    if (y1 > s->h)
        y1 = s->h;
    if (y0 >= y1)
        return;
    void *p = s->fb + y0 * s->pitch;
    u32 size = (y1 - y0) * s->pitch * 4;
    if (s->flush == 1)
        iosFlushDCache(p, size);
    else if (s->flush == 2)
        cpu_dcache_clean_range(p, size);
}

void surface_clear(surface_t *s)
{
    for (u32 y = 0; y < s->h; y++) {
        u32 *row = s->fb + y * s->pitch;
        for (u32 x = 0; x < s->w; x++)
            row[x] = COL_BG;
    }
    surface_flush(s, 0, s->h);
}

/* ---- layout --------------------------------------------------------------- */

u32 render_layout(char *cells, u32 rows, u32 cols)
{
    /* one spare row: a trailing newline opens an empty row that must not
     * overwrite the oldest row we still want to show */
    static char work[(TV_ROWS + 1) * TV_COLS];
    u32 need = rows * cols;            /* every display row consumes <= cols bytes */
    u32 wr = rows + 1;                 /* rows in the work ring */
    if (wr * cols > sizeof(work))
        return 0;

    u32 end = log_wpos;
    u32 look = 2 * need;
    if (look > LOG_RING_SIZE)
        look = LOG_RING_SIZE;
    if (look > end)
        look = end;
    u32 pos = end - look;

    /* Start at a line boundary if one exists that still leaves >= need bytes
     * (so >= rows rows); otherwise (one huge line) start mid-line. */
    if (pos != 0) {
        u32 limit = end > need ? end - need : 0;
        u32 p = pos;
        while (p < limit && log_ring[p & LOG_RING_MASK] != '\n')
            p++;
        pos = (p < limit) ? p + 1 : limit;
    }

    u32 n = 0, col = 0;
    memset(work, ' ', cols);
    for (; pos < end; pos++) {
        char c = log_ring[pos & LOG_RING_MASK];
        int nl = (c == '\n');
        if (nl || col == cols) {
            n++;
            col = 0;
            memset(&work[(n % wr) * cols], ' ', cols);
            if (nl)
                continue;
        }
        work[(n % wr) * cols + col++] = c;
    }

    u32 used = n + 1;
    if (col == 0 && n > 0)
        used = n;                    /* don't show an empty last row */
    u32 first = used > rows ? used - rows : 0;
    u32 shown = used > rows ? rows : used;

    for (u32 i = 0; i < rows; i++) {
        if (i < shown)
            memcpy(&cells[i * cols], &work[((first + i) % wr) * cols], cols);
        else
            memset(&cells[i * cols], ' ', cols);
    }
    return shown;
}

/* ---- full view ------------------------------------------------------------- */

static char cells[TV_ROWS * TV_COLS];
static char shadow[TV_ROWS * TV_COLS];
static int shadow_valid;

void render_invalidate(void)
{
    shadow_valid = 0;
}

void render_full(surface_t *s)
{
    render_layout(cells, TV_ROWS, TV_COLS);

    u32 ymin = s->h, ymax = 0;
    for (u32 r = 0; r < TV_ROWS; r++) {
        u32 y = MARGIN_Y + r * LINE_H;
        for (u32 c = 0; c < TV_COLS; c++) {
            u32 i = r * TV_COLS + c;
            if (shadow_valid && shadow[i] == cells[i])
                continue;
            draw_char(s, MARGIN_X + c * FONT_W, y, cells[i], COL_FG, COL_BG);
            shadow[i] = cells[i];
            if (y < ymin) ymin = y;
            if (y + LINE_H > ymax) ymax = y + LINE_H;
        }
    }
    shadow_valid = 1;
    surface_flush(s, ymin, ymax);
}

/* ---- incremental (MMU off, kern_main phase) --------------------------------- */

static u32 direct_pos, direct_row, direct_col;

static void direct_clear_row(surface_t *s, u32 row)
{
    u32 y = MARGIN_Y + row * LINE_H;
    for (u32 c = 0; c < TV_COLS; c++)
        draw_char(s, MARGIN_X + c * FONT_W, y, ' ', COL_FG, COL_BG);
}

static void direct_newline(surface_t *s)
{
    direct_col = 0;
    direct_row = (direct_row + 1) % TV_ROWS;
    direct_clear_row(s, direct_row);
    /* marker row below the newest line, so the wrap point is visible */
    u32 next = (direct_row + 1) % TV_ROWS;
    u32 y = MARGIN_Y + next * LINE_H;
    for (u32 c = 0; c < TV_COLS; c++)
        draw_char(s, MARGIN_X + c * FONT_W, y, '-', COL_FG, COL_BG);
}

void render_direct(surface_t *s)
{
    u32 end = log_wpos;
    if (end - direct_pos > LOG_RING_SIZE)
        direct_pos = end - LOG_RING_SIZE;
    for (; direct_pos < end; direct_pos++) {
        char c = log_ring[direct_pos & LOG_RING_MASK];
        if (c == '\n') {
            direct_newline(s);
            continue;
        }
        if (direct_col == TV_COLS)
            direct_newline(s);
        draw_char(s, MARGIN_X + direct_col * FONT_W, MARGIN_Y + direct_row * LINE_H,
                  c, COL_FG, COL_BG);
        direct_col++;
    }
    /* Only called privileged. Normally the D-cache is off here (MMU off);
     * if it isn't, write the pixels back so the display controller sees them. */
    if (cpu_cp15_ctrl_read() & CP15_DCACHE)
        cpu_dcache_clean_all();
    shadow_valid = 0;   /* the full view must repaint everything later */
}

/* ---- overlay ----------------------------------------------------------------- */

static char ovl_cells[OVL_ROWS * TV_COLS];

void render_overlay(surface_t *s)
{
    u32 cols = (s->w - 2 * MARGIN_X) / FONT_W;
    if (cols > TV_COLS)
        cols = TV_COLS;
    if (s->h < OVL_ROWS * LINE_H + 2 * MARGIN_Y || cols < 20)
        return;

    render_layout(ovl_cells, OVL_ROWS, cols);
    u32 y0 = s->h - MARGIN_Y - OVL_ROWS * LINE_H;

    /* black frame around the text so the box is opaque up to the edges */
    u32 xtext_end = MARGIN_X + cols * FONT_W;
    for (u32 y = y0 - MARGIN_Y / 2; y < s->h; y++) {
        u32 *row = s->fb + y * s->pitch;
        int text_row = y >= y0 && y < y0 + OVL_ROWS * LINE_H;
        for (u32 x = 0; x < s->w; x++) {
            if (text_row && x >= MARGIN_X && x < xtext_end)
                x = xtext_end;          /* text cells are drawn below */
            if (x < s->w)
                row[x] = COL_BG;
        }
    }
    for (u32 r = 0; r < OVL_ROWS; r++)
        draw_text(s, MARGIN_X, y0 + r * LINE_H, &ovl_cells[r * cols], cols, COL_FG, COL_BG);
    surface_flush(s, y0 - MARGIN_Y / 2, s->h);
}
