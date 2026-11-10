/*
 * wafel_screen_log - hardware access and display init.
 *
 * Register definitions and the display init sequence are taken from minute
 * (https://github.com/shinyquagsire23/minute_minute, source/gpu.c, gpu.h,
 * gpu_init.c, pll.c, i2c.c, asic.c, latte.h):
 *   Copyright (C) 2016 SALT, Copyright (C) 2016 Daz Jones <daz@dazzozo.com>,
 *   Copyright (C) 2023 Max Thomas <mtinc2@gmail.com>
 * Licensed under the GNU GPL, version 2.
 */
#ifndef SCRLOG_HW_H
#define SCRLOG_HW_H

#include <wafel/types.h>
#include <wafel/utils.h>

/* ---- Latte registers (physical; also mapped 1:1 into MCP by wafel_core) - */
#define LT_REG_BASE             (0x0D800000)
#define LT_AVE_I2C_INT_MASK     (LT_REG_BASE + 0x068)
#define LT_AVE_I2C_INT_STATE    (LT_REG_BASE + 0x06C)
#define LT_AVE_I2C_CLOCK        (LT_REG_BASE + 0x250)
#define LT_AVE_I2C_INOUT_DATA   (LT_REG_BASE + 0x254)
#define LT_AVE_I2C_INOUT_CTRL   (LT_REG_BASE + 0x258)
#define LT_RESETS               (LT_REG_BASE + 0x5E0)
#define LT_ABIF_OFFSET          (LT_REG_BASE + 0x620)
#define LT_ABIF_DATA            (LT_REG_BASE + 0x624)
#define LT_UNK628               (LT_REG_BASE + 0x628)

/* ---- GPU display controller registers (ABIF GPU space) ----------------- */
#define DC1_BASE (0x6000)

#define MCIF_CONTROL                            (0x6CB8)
#define DC_LUT_AUTOFILL                         (0x64A0)
#define D1CRTC_H_TOTAL                          (DC1_BASE + 0x000)
#define D1CRTC_H_BLANK_START_END                (DC1_BASE + 0x004)
#define D1CRTC_H_SYNC_A                         (DC1_BASE + 0x008)
#define D1CRTC_V_TOTAL                          (DC1_BASE + 0x020)
#define D1CRTC_V_BLANK_START_END                (DC1_BASE + 0x024)
#define D1CRTC_V_SYNC_A                         (DC1_BASE + 0x028)
#define D1CRTC_CONTROL                          (DC1_BASE + 0x080)
#define D1CRTC_INTERLACE_CONTROL                (DC1_BASE + 0x088)
#define D1CRTC_OVERSCAN_COLOR                   (DC1_BASE + 0x094)
#define D1CRTC_BLACK_COLOR                      (DC1_BASE + 0x098)
#define D1MODE_MASTER_UPDATE_LOCK               (DC1_BASE + 0x0E0)
#define D1CRTC_UPDATE_LOCK                      (DC1_BASE + 0x0E8)
#define D1GRPH_ENABLE                           (DC1_BASE + 0x100)
#define D1GRPH_CONTROL                          (DC1_BASE + 0x104)
#define D1GRPH_SWAP_CNTL                        (DC1_BASE + 0x10C)
#define D1GRPH_PRIMARY_SURFACE_ADDRESS          (DC1_BASE + 0x110)
#define D1GRPH_PRIMARY_SURFACE_ADDRESS_HI       (DC1_BASE + 0x114)
#define D1GRPH_SECONDARY_SURFACE_ADDRESS        (DC1_BASE + 0x118)
#define D1GRPH_SECONDARY_SURFACE_ADDRESS_HI     (DC1_BASE + 0x11C)
#define D1GRPH_PITCH                            (DC1_BASE + 0x120)
#define D1GRPH_X_START                          (DC1_BASE + 0x12C)
#define D1GRPH_Y_START                          (DC1_BASE + 0x130)
#define D1GRPH_X_END                            (DC1_BASE + 0x134)
#define D1GRPH_Y_END                            (DC1_BASE + 0x138)
#define D1OVL_COLOR_MATRIX_TRANSFORMATION_CNTL  (DC1_BASE + 0x140)
#define D1GRPH_UPDATE                           (DC1_BASE + 0x144)
#define D1OVL_ENABLE                            (DC1_BASE + 0x180)
#define D1OVL_UPDATE                            (DC1_BASE + 0x1AC)
#define D1GRPH_ALPHA                            (DC1_BASE + 0x304)
#define D1OVL_ALPHA                             (DC1_BASE + 0x308)
#define D1OVL_ALPHA_CONTROL                     (DC1_BASE + 0x30C)
#define D1GRPH_COLOR_MATRIX_TRANSFORMATION_CNTL (DC1_BASE + 0x380)

/* minute's TV framebuffer (MEM2) and mode: 1280x720 surface, 1080p timing */
#define FB_TV_ADDR   (0x14000000 + 0x3500000)
#define FB_TV_W      (1280)
#define FB_TV_H      (720)
#define FB_TV_CTRL   (0x100002)   /* 32bpp, linear aligned */
#define CRTC_H_TOTAL (2199)
#define CRTC_V_TOTAL (1124)

u32  abif_gpu_read32(u32 offset);
void abif_gpu_write32(u32 offset, u32 value32);

/* Full display init (minute gpu_display_init). Safe with the MMU off
 * (kernel, physical addresses) and from MCP (registers mapped 1:1). */
void display_init(void);

typedef struct {
    u32 surface, pitch, control, xend, yend, htotal, vtotal, grph_en, crtc, swap;
} dc_state_t;
void display_read_state(dc_state_t *s);

/* D1 is enabled, CRTC running, 32bpp linear surface in MEM2 with sane size */
int dc_state_usable(const dc_state_t *d);
/* same scanout configuration (address, pitch, format, size, timing) */
int dc_state_same(const dc_state_t *a, const dc_state_t *b);

#endif
