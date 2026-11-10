/*
 * wafel_screen_log - display init, ported from minute
 * (source/gpu.c gpu_display_init / gpu_idk_upll / gpu_do_*_list,
 *  source/pll.c pll_vi1_write / pll_vi2_write / pll_upll_*,
 *  source/i2c.c ave_i2c_*, source/asic.c abif_*).
 *   Copyright (C) 2016 SALT, Copyright (C) 2016 Daz Jones <daz@dazzozo.com>,
 *   Copyright (C) 2023 Max Thomas <mtinc2@gmail.com>
 * Licensed under the GNU GPL, version 2.
 *
 * Kept as close to minute as possible: this exact sequence is what minute's
 * menu uses on hardware. Differences: printf removed, minute's gpio code
 * replaced by wafel_core's identical gpio_ave_i2c_init().
 */
#include "hw.h"
#include "gpu_tables.h"
#include <string.h>

/* wafel_core exports this (latte/gpio.c, same code as minute's) */
LINKABLE void gpio_ave_i2c_init(void);

#define MEM_GPU_ENDIANNESS (0x0D8B4000 + 0x64A)

/* ---- ABIF (asic.c) ------------------------------------------------------ */
static void abif_force_data_read_cycle(void)
{
    read32(LT_ABIF_DATA);
}

static u32 abif_cpl_ct_read32(u32 offset)
{
    u32 ret = 0;
    write32(LT_ABIF_OFFSET, offset);
    ret |= read32(LT_ABIF_DATA) & 0xFFFF;
    write32(LT_ABIF_OFFSET, (offset & 0xFFFF) + 2);
    ret |= read32(LT_ABIF_DATA) << 16;
    return ret;
}

static u16 abif_cpl_tr_read16(u32 offset)
{
    write32(LT_ABIF_OFFSET, (offset & 0xFFFF) | 0x01000000);
    return read32(LT_ABIF_DATA) & 0xFFFF;
}

u32 abif_gpu_read32(u32 offset)
{
    write32(LT_ABIF_OFFSET, (offset & 0xFFFF) | 0xC0000000);
    return read32(LT_ABIF_DATA);
}

static void abif_cpl_ct_write32(u32 offset, u32 value32)
{
    write32(LT_ABIF_OFFSET, (offset & 0xFFFF));
    write32(LT_ABIF_DATA, value32 & 0xFFFF);
    abif_force_data_read_cycle();
    write32(LT_ABIF_OFFSET, (offset & 0xFFFF) + 2);
    write32(LT_ABIF_DATA, value32 >> 16);
    abif_force_data_read_cycle();
}

static void abif_cpl_tr_write16(u32 offset, u16 value16)
{
    write32(LT_ABIF_OFFSET, (offset & 0xFFFF) | 0x01000000);
    write32(LT_ABIF_DATA, value16);
    abif_force_data_read_cycle();
}

void abif_gpu_write32(u32 offset, u32 value32)
{
    write32(LT_ABIF_OFFSET, (offset & 0xFFFF) | 0xC0000000);
    write32(LT_ABIF_DATA, value32);
    abif_force_data_read_cycle();
}

static void abif_gpu_mask32(u32 offset, u32 clear, u32 set)
{
    u32 val = abif_gpu_read32(offset);
    val &= ~clear;
    val |= set;
    abif_gpu_write32(offset, val);
}

/* ---- PLLs (pll.c) ------------------------------------------------------- */
typedef struct {
    u32 fastEn, dithEn, satEn, ssEn, bypOut, bypVco, operational;
    u16 clkR, clkFMsb, clkFLsb, clkO0Div, clkO1Div, clkO2Div, clkS, clkVMsb, clkVLsb;
    u32 bwAdj, options;
} bsp_pll_cfg;

//                       fastEn dithEn satEn ssEn bypOut bypVco operational clkR clkFMsb clkFLsb clkO0Div clkO1Div clkO2Div clkS   clkVMsb clkVLsb bwAdj options
static const bsp_pll_cfg vi1_pllcfg = {0, 1, 1, 0, 0, 0, 0, 0xC, 0x1F4, 0, 0xC,  0xC, 0xC,  0,     0, 0x0, 0x3D, 0x1C};
static const bsp_pll_cfg vi2_pllcfg = {0, 1, 1, 0, 0, 0, 0, 0,   0x2C,  0, 0x2C, 0x4, 0x2C, 0,     0, 0x0, 0x4,  0x0};
static const bsp_pll_cfg upll_cfg   = {0, 1, 1, 0, 0, 0, 1, 0,   0x48,  0, 0x8,  0xA, 0x54, 0x1C2, 0, 0xD, 0x8,  0x0};

static void pll_vi1_write(const bsp_pll_cfg *pCfg)
{
    udelay(10);
    abif_cpl_tr_write16(2, pCfg->options & 0x1E);
    abif_cpl_tr_write16(0x10, pCfg->clkR | 0x8000 | (u16)(pCfg->clkO0Div << 6));
    abif_cpl_tr_write16(0x12, (pCfg->bypVco ? 0x1000 : 0) | (pCfg->bypOut ? 0x2000 : 0) | pCfg->clkFMsb);
    abif_cpl_tr_write16(0x14, (pCfg->satEn ? 0x800 : 0) | (pCfg->fastEn ? 0x1000 : 0) | pCfg->clkO2Div);
    abif_cpl_tr_write16(0x16, pCfg->clkO1Div);
    abif_cpl_tr_write16(0x18, pCfg->clkVLsb);
    abif_cpl_tr_write16(0x1A, (pCfg->ssEn ? 0x400 : 0) | (pCfg->dithEn ? 0x800 : 0) | pCfg->clkVMsb);
    abif_cpl_tr_write16(0x1C, pCfg->clkS);
    abif_cpl_tr_write16(0x1E, pCfg->bwAdj);
    abif_cpl_tr_write16(0x20, pCfg->clkFLsb);
    udelay(5);
    abif_cpl_tr_write16(0x14, (pCfg->satEn ? 0x4800 : 0x4000) | (pCfg->fastEn ? 0x1000 : 0) | pCfg->clkO2Div);
    udelay(50);
    abif_cpl_tr_write16(0x14, (pCfg->satEn ? 0xC800 : 0xC000) | (pCfg->fastEn ? 0x1000 : 0) | pCfg->clkO2Div);
    udelay(50);
}

static void pll_vi2_write(const bsp_pll_cfg *pCfg)
{
    udelay(10);
    abif_cpl_tr_write16(0x30, pCfg->clkR | 0x8000 | (u16)(pCfg->clkO0Div << 6));
    abif_cpl_tr_write16(0x32, (pCfg->bypVco ? 0x1000 : 0) | (pCfg->bypOut ? 0x2000 : 0) | pCfg->clkFMsb);
    abif_cpl_tr_write16(0x34, (pCfg->satEn ? 0x800 : 0) | (pCfg->fastEn ? 0x1000 : 0) | pCfg->clkO2Div);
    abif_cpl_tr_write16(0x36, pCfg->clkO1Div);
    abif_cpl_tr_write16(0x24, pCfg->clkVLsb);
    abif_cpl_tr_write16(0x26, (pCfg->ssEn ? 0x400 : 0) | (pCfg->dithEn ? 0x800 : 0) | pCfg->clkVMsb);
    abif_cpl_tr_write16(0x28, pCfg->clkS);
    abif_cpl_tr_write16(0x2A, pCfg->bwAdj);
    abif_cpl_tr_write16(0x2C, pCfg->clkFLsb);
    udelay(5);
    abif_cpl_tr_write16(0x34, (pCfg->satEn ? 0x4800 : 0x4000) | (pCfg->fastEn ? 0x1000 : 0) | pCfg->clkO2Div);
    udelay(50);
    abif_cpl_tr_write16(0x34, (pCfg->satEn ? 0xC800 : 0xC000) | (pCfg->fastEn ? 0x1000 : 0) | pCfg->clkO2Div);
    udelay(50);
}

static void pll_upll_write(const bsp_pll_cfg *pCfg)
{
    abif_cpl_ct_write32(0x888u, 0x2001u);
    abif_cpl_ct_write32(0x878u, pCfg->clkR | (pCfg->clkFMsb << 6) | 0x18000000 | (pCfg->clkO0Div << 18));
    udelay(5);
    abif_cpl_ct_write32(0x878u, pCfg->clkR | (pCfg->clkFMsb << 6) | 0x98000000 | (pCfg->clkO0Div << 18));
    udelay(10);
    abif_cpl_tr_write16(0, pCfg->clkO2Div);
    abif_cpl_ct_write32(0x87Cu, (pCfg->satEn ? 0x8000000 : 0) | (pCfg->fastEn ? 0x10000000 : 0)
                                | pCfg->clkO1Div | (pCfg->clkFLsb << 9));
    abif_cpl_ct_write32(0x880u, (pCfg->ssEn ? 0x4000000 : 0) | (pCfg->dithEn ? 0x8000000 : 0)
                                | pCfg->clkVLsb | (pCfg->clkVMsb << 16));
    abif_cpl_ct_write32(0x884u, pCfg->clkS | (pCfg->bwAdj << 12));
    udelay(5);
    abif_cpl_ct_write32(0x878u, pCfg->clkR | (pCfg->clkFMsb << 6) | 0x18000000 | (pCfg->clkO0Div << 18));
    udelay(200);
    abif_cpl_ct_write32(0x878u, (pCfg->bypVco ? 0x8000000 : 0) | (pCfg->bypOut ? 0x10000000 : 0)
                                | pCfg->clkR | (pCfg->clkFMsb << 6) | (pCfg->clkO0Div << 18));
    abif_cpl_ct_write32(0x888u, 0x4002u);
}

static void pll_upll_read(bsp_pll_cfg *pOut)
{
    u32 v;
    memset(pOut, 0, sizeof(*pOut));

    v = abif_cpl_ct_read32(0x878u);
    pOut->clkR = v & 0x3F;
    pOut->bypVco = !!(v & 0x8000000u);
    pOut->bypOut = !!(v & 0x10000000u);
    pOut->clkFMsb = (v >> 6) & 0x3FFF;
    pOut->clkO0Div = (v >> 18) & 0x1FF;
    v = abif_cpl_ct_read32(0x87Cu);
    pOut->clkFLsb = (v >> 9) & 0x3FFF;
    pOut->clkO1Div = v & 0x1FF;
    pOut->satEn = !!(v & 0x8000000);
    pOut->fastEn = !!(v & 0x10000000);
    v = abif_cpl_ct_read32(0x880u);
    pOut->clkVMsb = (v >> 16) & 0x3FFF;
    pOut->clkVLsb = v & 0xFFFF;
    pOut->ssEn = !!(v & 0x4000000u);
    pOut->dithEn = !!(v & 0x8000000);
    v = abif_cpl_ct_read32(0x884u);
    pOut->bwAdj = (v & 0xFFF000u) >> 12;
    pOut->clkO2Div = abif_cpl_tr_read16(0) & 0x1FF;
    v = abif_cpl_ct_read32(0x878u);
    if ((v & 0x20000000) || (v & 0x10000000) || ((v & 0x8000000u) >> 27)
        || ((abif_cpl_ct_read32(0x888u) & 0xE000) != 0x4000 && (abif_cpl_ct_read32(0x888u) & 7) != 2))
        return;
    pOut->operational = 1;
}

/* gpu.c gpu_idk_upll(): same as IOS-BSP FUN_e6003c70 (GFX subsystem init) */
static void gpu_idk_upll(void)
{
    u32 val_5E0 = read32(LT_RESETS);
    if ((val_5E0 & 4) == 0) {
        write32(LT_RESETS, val_5E0 | 4);
        udelay(10);
    }
    abif_gpu_mask32(0x5930u, 0x1FF, 0x49);
    abif_gpu_mask32(0x5938u, 0x1FF, 0x49);
    set32(LT_UNK628, 1u);
}

/* pll.c pll_upll_init(): same as IOS-BSP bsp_GFX_subsystem_init, normal power mode */
static void pll_upll_init(void)
{
    bsp_pll_cfg cfg;
    gpu_idk_upll();
    pll_upll_read(&cfg);
    if (memcmp(&upll_cfg, &cfg, sizeof(bsp_pll_cfg)))
        pll_upll_write(&upll_cfg);
}

/* ---- AVE I2C (i2c.c) ---------------------------------------------------- */
static void ave_i2c_init(u32 clock, u32 channel)
{
    if (gpio_ave_i2c_init)
        gpio_ave_i2c_init();
    write32(LT_AVE_I2C_CLOCK, ((channel << 1) & 0xFFFF) | (((243000000 / 2) / clock) << 16) | 1);
    write32(LT_AVE_I2C_INOUT_CTRL, 0);
}

static void ave_i2c_inout_data(u8 data, int last)
{
    write32(LT_AVE_I2C_INOUT_DATA, data | (last ? 1 << 8 : 0));
    write32(LT_AVE_I2C_INOUT_CTRL, 1);
}

static int ave_i2c_wait_xfer_done(void)
{
    u32 mask = 0;
    for (int i = 0; i < 4999; i++) {
        mask = read32(LT_AVE_I2C_INT_STATE) & read32(LT_AVE_I2C_INT_MASK);
        if (mask & 0xC80) {
            clear32(LT_AVE_I2C_INT_STATE, ~read32(LT_AVE_I2C_INT_MASK));
            return -2;
        }
        if (mask & 0x060) {
            clear32(LT_AVE_I2C_INT_STATE, ~read32(LT_AVE_I2C_INT_MASK));
            return 0;
        }
        udelay(1000);
    }
    write32(LT_AVE_I2C_INT_STATE, mask);
    return -1;
}

static int ave_i2c_write(u8 slave_7bit, const u8 *data, size_t size)
{
    int res;
    if (!data || size == 0 || size > 0x40)
        return -4;

    write32(LT_AVE_I2C_INT_STATE, 0xCC0);
    set32(LT_AVE_I2C_INT_MASK, 0xCC0);
    ave_i2c_inout_data(slave_7bit << 1, 0);
    while (size--)
        ave_i2c_inout_data(*data++, size == 0);
    res = ave_i2c_wait_xfer_done();
    clear32(LT_AVE_I2C_INT_MASK, 0xCC0);
    return res;
}

/* ---- gpu.c ---------------------------------------------------------------- */
static void gpu_do_init_list(const gpu_init_entry_t *paEntries, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        const gpu_init_entry_t *entry = &paEntries[i];
        if (entry->clear_bits) {
            u32 val = abif_gpu_read32(entry->addr);
            val &= ~entry->clear_bits;
            val |= entry->set_bits;
            abif_gpu_write32(entry->addr, val);
        } else {
            abif_gpu_write32(entry->addr, entry->set_bits);
        }
        if (entry->usec_delay)
            udelay(entry->usec_delay);
    }
}

static void gpu_do_ave_list(const ave_init_entry_t *paEntries, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        const ave_init_entry_t *entry = &paEntries[i];
        u8 tmp[2] = { entry->reg_idx, entry->value };
        ave_i2c_write(entry->addr, tmp, 2);
        if (entry->usec_delay)
            udelay(entry->usec_delay);
    }
}

void display_init(void)
{
    ave_i2c_init(400000, 0);

    pll_vi1_write(&vi1_pllcfg);
    pll_vi2_write(&vi2_pllcfg);

    /* QUIRK (as in minute): gpu_init_entries_C is declared [0x65] but lists 100
     * entries, so the last iteration writes GPU register 0 with 0. Kept because
     * this exact sequence is what minute runs on hardware. */
    gpu_do_init_list(gpu_init_entries_C, NUM_GPU_ENTRIES_C);
    gpu_do_ave_list(ave_init_entries_C, NUM_AVE_ENTRIES_C);

    abif_gpu_write32(D1GRPH_PRIMARY_SURFACE_ADDRESS, FB_TV_ADDR);

    /* minute's endianness hack */
    if ((read16(MEM_GPU_ENDIANNESS) & 3) != 2)
        abif_gpu_write32(D1GRPH_SWAP_CNTL, 0x222);
    else
        abif_gpu_write32(D1GRPH_SWAP_CNTL, 0x220);

    pll_upll_init();
}

void display_read_state(dc_state_t *s)
{
    s->surface = abif_gpu_read32(D1GRPH_PRIMARY_SURFACE_ADDRESS);
    s->pitch   = abif_gpu_read32(D1GRPH_PITCH);
    s->control = abif_gpu_read32(D1GRPH_CONTROL);
    s->xend    = abif_gpu_read32(D1GRPH_X_END);
    s->yend    = abif_gpu_read32(D1GRPH_Y_END);
    s->htotal  = abif_gpu_read32(D1CRTC_H_TOTAL);
    s->vtotal  = abif_gpu_read32(D1CRTC_V_TOTAL);
    s->grph_en = abif_gpu_read32(D1GRPH_ENABLE);
    s->crtc    = abif_gpu_read32(D1CRTC_CONTROL);
    s->swap    = abif_gpu_read32(D1GRPH_SWAP_CNTL);
}

int dc_state_usable(const dc_state_t *d)
{
    u32 addr = d->surface & ~0xFFu;
    if (!(d->grph_en & 1) || !(d->crtc & 1))
        return 0;
    /* GRPH_DEPTH [1:0] == 2 (32bpp), GRPH_ARRAY_MODE [23:20] <= 1 (linear) */
    if ((d->control & 3) != 2 || ((d->control >> 20) & 0xF) > 1)
        return 0;
    if (d->xend < 320 || d->xend > 1920 || d->yend < 240 || d->yend > 1080 || d->pitch < d->xend)
        return 0;
    if (addr < 0x10000000 || addr + d->pitch * d->yend * 4 > 0x20000000)
        return 0;
    return 1;
}

int dc_state_same(const dc_state_t *a, const dc_state_t *b)
{
    return (a->surface & ~0xFFu) == (b->surface & ~0xFFu)
        && a->pitch == b->pitch && a->control == b->control
        && a->xend == b->xend && a->yend == b->yend
        && a->htotal == b->htotal && a->vtotal == b->vtotal
        && (a->grph_en & 1) == (b->grph_en & 1) && (a->crtc & 1) == (b->crtc & 1);
}
