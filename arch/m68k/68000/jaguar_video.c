// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tom text console for the Jag.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/types.h>

// Courtesy of a random hello world I found for the Jaguar of which all of this is based
#include "font.h"
// Courtesy of linuxmd
#include "tuxhead.h"

#define TOM_BASE 0xF00000UL
#define TOMREG(off) (*(volatile u16 *)(TOM_BASE + (off)))
#define TOMREG32(off) (*(volatile u32 *)(TOM_BASE + (off)))

#define VC_ADDR (TOM_BASE + 0x06)
#define OLPLO TOMREG(0x20)
#define OLPHI TOMREG(0x22)
#define VMODE TOMREG(0x28)
#define BORD1 TOMREG(0x2A)
#define BORD2 TOMREG(0x2C)
#define HDB1 TOMREG(0x38)
#define HDB2 TOMREG(0x3A)
#define HDE TOMREG(0x3C)
#define VDB TOMREG(0x46)
#define VDE TOMREG(0x48)
#define VI TOMREG(0x4E)
#define BG TOMREG(0x58)
#define INT1 TOMREG(0xE0)
#define CLUT(n) (*(volatile u16 *)(TOM_BASE + 0x400 + 2 * (n)))

#define G_FLAGS TOMREG32(0x2100)
#define G_END TOMREG32(0x210C)
#define G_PC TOMREG32(0x2110)
#define G_CTRL TOMREG32(0x2114)
#define G_RAM (TOM_BASE + 0x3000)

#define GPUGO 0x00000001

#define VC_COUNT_MASK 0x07FF

#define JOY_CONFIG (*(volatile u16 *)0xF14002UL)
#define VIDTYPE 0x0010

#define VMODE_VAL 0x06C7

#define C_VIDENA 0x0001

#define NTSC_WIDTH 1409
#define NTSC_HMID 823
#define NTSC_HEIGHT 241
#define NTSC_VMID 266

#define PAL_WIDTH 1381
#define PAL_HMID 843
#define PAL_HEIGHT 287
#define PAL_VMID 322

#define BRANCHOBJ 3
#define STOPOBJ 4
#define O_BRGT (1 << 14)
#define O_BRLT (2 << 14)
#define O_STOPINTS 0x00000008
#define O_DEPTH8 (3 << 12)
#define O_NOGAP (1 << 15)

#ifndef T_YREZ
#define T_YREZ 200
#endif
// From font.h
#define FB_WIDTH T_XREZ
#define FB_HEIGHT T_YREZ
#define FB_SIZE (FB_WIDTH * FB_HEIGHT)
#define FB_PHRASES (FB_WIDTH / 8)

#define ROW_BYTES (F_HEIGHT * FB_WIDTH)

#define BANNER_H 32
#define BANNER_SIZE (FB_WIDTH * BANNER_H)

#define TUX_CLUT_BASE 16

#define FB_SLACK FB_SIZE

static u8 jag_fb[FB_SIZE + FB_SLACK] __aligned(16);
static u8 jag_banner[BANNER_SIZE] __aligned(16);
static unsigned int fb_base;
static u32 jag_olist[14] __aligned(32);

#define OL_BANNER 4
#define OL_TEXT 8
#define OL_STOP 12

static u32 jag_bmp_hi, jag_bmp_lo;
static u32 jag_ban_hi, jag_ban_lo;
static u32 jag_link_hi, jag_link_lo;

static u16 disp_width, disp_height;
static u16 a_vdb, a_vde;

static bool jag_video_up;

static DEFINE_SPINLOCK(jag_lock);

extern unsigned short jaguar_int1_enable;

static void jaguar_video_refresh(void)
{
    *(volatile u32 *)&jag_olist[OL_TEXT] = jag_bmp_hi;
    *(volatile u32 *)&jag_olist[OL_TEXT + 1] = jag_bmp_lo;
    *(volatile u32 *)&jag_olist[OL_BANNER] = jag_ban_hi;
    *(volatile u32 *)&jag_olist[OL_BANNER + 1] = jag_ban_lo;
}

void jaguar_video_vblank(void)
{
    jaguar_video_refresh();
}

#define GINS(op, r1, r2) ((u16)(((op) << 10) | (((r1) & 31) << 5) | ((r2) & 31)))

#define G_OP_AND 9
#define G_OP_CMP 30
#define G_OP_CMPQ 31
#define G_OP_MOVEQ 35
#define G_OP_MOVEI 38
#define G_OP_LOADW 40
#define G_OP_LOAD 41
#define G_OP_STORE 47
#define G_OP_JUMP 52
#define G_OP_JR 53
#define G_OP_NOP 57

#define GCC_T 0
#define GCC_EQ 0x02
#define GCC_PL 0x14
#define GCC_MI 0x18

// Could've used the TOM RISC assembler but eh
static unsigned int gpu_emit_movei(u16 *p, unsigned int n, u32 imm, int rd)
{
    p[n++] = GINS(G_OP_MOVEI, 0, rd);
    p[n++] = (u16)imm;
    p[n++] = (u16)(imm >> 16);
    return n;
}

// Same here lol (Or a blob, even)
static bool __init jaguar_gpu_refresher_start(void)
{
    u16 prog[80];
    unsigned int i, n = 0;

    n = gpu_emit_movei(prog, n, VC_ADDR, 0);
    n = gpu_emit_movei(prog, n, a_vdb, 1);
    n = gpu_emit_movei(prog, n, (u32)a_vde + 1, 2);
    n = gpu_emit_movei(prog, n, VC_COUNT_MASK, 7);
    n = gpu_emit_movei(prog, n, (u32)(unsigned long)&jag_bmp_hi, 10);
    n = gpu_emit_movei(prog, n, (u32)(unsigned long)&jag_bmp_lo, 11);
    n = gpu_emit_movei(prog, n, (u32)(unsigned long)&jag_olist[OL_TEXT], 12);
    n = gpu_emit_movei(prog, n, (u32)(unsigned long)&jag_olist[OL_TEXT + 1], 13);
    n = gpu_emit_movei(prog, n, (u32)(unsigned long)&jag_ban_hi, 16);
    n = gpu_emit_movei(prog, n, (u32)(unsigned long)&jag_ban_lo, 17);
    n = gpu_emit_movei(prog, n, (u32)(unsigned long)&jag_olist[OL_BANNER], 18);
    n = gpu_emit_movei(prog, n, (u32)(unsigned long)&jag_olist[OL_BANNER + 1], 19);
    n = gpu_emit_movei(prog, n, G_RAM + (n + 3 + 1) * 2, 9);
    prog[n++] = GINS(G_OP_MOVEQ, 1, 8);
    prog[n++] = GINS(G_OP_LOADW, 0, 6);
    prog[n++] = GINS(G_OP_AND, 7, 6);
    prog[n++] = GINS(G_OP_CMP, 1, 6);
    prog[n++] = GINS(G_OP_JR, 7, GCC_MI);
    prog[n++] = GINS(G_OP_NOP, 0, 0);
    prog[n++] = GINS(G_OP_CMP, 2, 6);
    prog[n++] = GINS(G_OP_JR, 4, GCC_PL);
    prog[n++] = GINS(G_OP_NOP, 0, 0);
    prog[n++] = GINS(G_OP_MOVEQ, 1, 8);
    prog[n++] = GINS(G_OP_JUMP, 9, GCC_T);
    prog[n++] = GINS(G_OP_NOP, 0, 0);
    prog[n++] = GINS(G_OP_CMPQ, 0, 8);
    prog[n++] = GINS(G_OP_JR, -13 & 31, GCC_EQ);
    prog[n++] = GINS(G_OP_NOP, 0, 0);
    prog[n++] = GINS(G_OP_LOAD, 10, 4);
    prog[n++] = GINS(G_OP_LOAD, 11, 5);
    prog[n++] = GINS(G_OP_STORE, 12, 4);
    prog[n++] = GINS(G_OP_STORE, 13, 5);
    prog[n++] = GINS(G_OP_LOAD, 16, 4);
    prog[n++] = GINS(G_OP_LOAD, 17, 5);
    prog[n++] = GINS(G_OP_STORE, 18, 4);
    prog[n++] = GINS(G_OP_STORE, 19, 5);
    prog[n++] = GINS(G_OP_MOVEQ, 0, 8);
    prog[n++] = GINS(G_OP_JUMP, 9, GCC_T);
    prog[n++] = GINS(G_OP_NOP, 0, 0);

    if (n & 1)
    {
        prog[n++] = GINS(G_OP_NOP, 0, 0);
    }

    G_CTRL = 0;
    G_FLAGS = 0;
    G_END = 0x00070007;

    // Not the best w/a for Tom-local SRAM (It wants 32-bit wide accesses)
    for (i = 0; i < n; i += 2)
    {
        *(volatile u32 *)(G_RAM + i * 2) = ((u32)prog[i] << 16) | prog[i + 1];
    }

    G_PC = G_RAM;
    G_CTRL = GPUGO;

    return (G_CTRL & GPUGO) != 0;
}

// Probably could've converted the bytes to RGB16 but it maps well enough to do in runtime
static u16 __init md_to_jag_rgb(u16 md)
{
    unsigned int r = (md >> 1) & 7;
    unsigned int g = (md >> 5) & 7;
    unsigned int b = (md >> 9) & 7;
    unsigned int r5 = (r << 2) | (r >> 1);
    unsigned int b5 = (b << 2) | (b >> 1);
    unsigned int g6 = (g << 3) | g;

    return (r5 << 11) | (b5 << 6) | g6;
}

static void __init jag_draw_banner(void)
{
    unsigned int t, row, bx, x0 = 4, y0 = 0;

    for (t = 0; t < 16; t++)
    {
        u16 md = ((u16)tuxhead_pal[2 * t] << 8) | tuxhead_pal[2 * t + 1];

        CLUT(TUX_CLUT_BASE + t) = md_to_jag_rgb(md);
    }

    for (t = 0; t < 16; t++)
    {
        u16 entry = ((u16)tuxhead_tilemap[2 * t] << 8) | tuxhead_tilemap[2 * t + 1];
        const unsigned char *td = &tuxhead_tiles[(entry & 0x7FF) * 32];
        unsigned int tx = (t & 3) * 8, ty = (t >> 2) * 8;

        for (row = 0; row < 8; row++)
        {
            u8 *dst = &jag_banner[(y0 + ty + row) * FB_WIDTH + x0 + tx];

            for (bx = 0; bx < 4; bx++)
            {
                u8 v = td[row * 4 + bx];
                u8 hi = v >> 4, lo = v & 15;

                dst[bx * 2] = hi ? TUX_CLUT_BASE + hi : 0;
                dst[bx * 2 + 1] = lo ? TUX_CLUT_BASE + lo : 0;
            }
        }
    }

    // The damned white line
    // memset(&jag_banner[(BANNER_H - 1) * FB_WIDTH], 1, FB_WIDTH);
}

// From now on, heavily inspired by https://forums.atariage.com/topic/226336-virtual-jaguar-hello-world/
static unsigned int cur_col;
static unsigned int cur_y;

static void jag_draw_char(unsigned int x, unsigned int y, unsigned char ch)
{
    unsigned int charloc = (unsigned int)ch * F_CHARSIZE;
    u8 *dst = &jag_fb[fb_base + y * FB_WIDTH + x];
    unsigned int row, bit;

    for (row = 0; row < F_HEIGHT; row++)
    {
        unsigned char bits = textfont[charloc + row];

        for (bit = 0; bit < F_WIDTH; bit++)
        {
            dst[bit] = (bits >> (7 - bit)) & 1;
        }
        dst += FB_WIDTH;
    }
}

static void jag_update_bmp_pointer(void)
{
    jag_bmp_hi = jag_link_hi | (((u32)(unsigned long)jag_fb + fb_base) << 8);
}

static void jag_scroll(void)
{
    if (fb_base + ROW_BYTES > FB_SLACK)
    {
        memcpy(jag_fb, jag_fb + fb_base + ROW_BYTES, FB_SIZE - ROW_BYTES);
        fb_base = 0;
    }
    else
    {
        fb_base += ROW_BYTES;
    }

    memset(jag_fb + fb_base + FB_SIZE - ROW_BYTES, 0, ROW_BYTES);
    jag_update_bmp_pointer();
}

static void jag_newline(void)
{
    cur_col = 0;
    cur_y += F_HEIGHT;

    if (cur_y + F_HEIGHT > FB_HEIGHT)
    {
        jag_scroll();
        cur_y -= F_HEIGHT;
    }
}

static void jag_putc(char c)
{
    static enum
    {
        ESC_NONE,
        ESC_SEEN,
        ESC_CSI
    } esc_state;
    unsigned char ch = c;

    if (esc_state == ESC_SEEN)
    {
        esc_state = (ch == '[') ? ESC_CSI : ESC_NONE;
        return;
    }
    if (esc_state == ESC_CSI)
    {
        if (ch >= 0x40 && ch <= 0x7E)
            esc_state = ESC_NONE;
        return;
    }

    switch (ch)
    {
    case 0x1B:
        esc_state = ESC_SEEN;
        return;
    case '\n':
        jag_newline();
        return;
    case '\r':
        cur_col = 0;
        return;
    case '\t':
        do
        {
            jag_putc(' ');
        } while (cur_col & 7);
        return;
    case '\b':
        if (cur_col)
            cur_col--;
        return;
    case '\a':
        return;
    }

    if (ch < 0x20 || ch > 0x7E)
        ch = '?';

    if ((cur_col + 1) * F_WIDTH > FB_WIDTH)
        jag_newline();

    jag_draw_char(cur_col * F_WIDTH, cur_y, ch);
    cur_col++;
}

static void jag_puts(const char *s, unsigned int count)
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        jag_putc(s[i]);
    }
}

void jaguar_video_mirror_putc(unsigned char ch)
{
    unsigned long flags;

    if (!jag_video_up)
        return;

    spin_lock_irqsave(&jag_lock, flags);
    jag_putc(ch);
    spin_unlock_irqrestore(&jag_lock, flags);
}

static struct tty_driver *jag_tty_driver;
static struct tty_port jag_tty_port;

static int jag_tty_open(struct tty_struct *tty, struct file *filp)
{
    return tty_port_open(&jag_tty_port, tty, filp);
}

static void jag_tty_close(struct tty_struct *tty, struct file *filp)
{
    tty_port_close(&jag_tty_port, tty, filp);
}

static ssize_t jag_tty_write(struct tty_struct *tty, const u8 *buf, size_t count)
{
    unsigned long flags;

    spin_lock_irqsave(&jag_lock, flags);
    jag_puts(buf, count);
    spin_unlock_irqrestore(&jag_lock, flags);

    return count;
}

static unsigned int jag_tty_write_room(struct tty_struct *tty)
{
    return FB_WIDTH / F_WIDTH;
}

static const struct tty_operations jag_tty_ops = {
    .open = jag_tty_open,
    .close = jag_tty_close,
    .write = jag_tty_write,
    .write_room = jag_tty_write_room,
};

static int __init jaguar_video_tty_init(void)
{
    int ret;

    if (!jag_video_up)
    {
        return 0;
    }

    tty_port_init(&jag_tty_port);

    jag_tty_driver = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);

    if (IS_ERR(jag_tty_driver))
    {
        ret = PTR_ERR(jag_tty_driver);
        goto err_port;
    }

    jag_tty_driver->driver_name = "jaguar_video";
    jag_tty_driver->name = "ttyV";
    jag_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
    jag_tty_driver->subtype = SERIAL_TYPE_NORMAL;
    jag_tty_driver->init_termios = tty_std_termios;
    tty_set_operations(jag_tty_driver, &jag_tty_ops);
    tty_port_link_device(&jag_tty_port, jag_tty_driver, 0);

    ret = tty_register_driver(jag_tty_driver);

    if (ret)
    {
        tty_driver_kref_put(jag_tty_driver);
        jag_tty_driver = NULL;
        goto err_port;
    }

    tty_register_device(jag_tty_driver, 0, NULL);

    return 0;

err_port:
    tty_port_destroy(&jag_tty_port);
    pr_err("jaguar_video: tty registration failed: %d\n", ret);
    return ret;
}
device_initcall(jaguar_video_tty_init);

static void jag_vidcon_write(struct console *co, const char *s, unsigned int count)
{
    unsigned long flags;
    int locked = 1;

    if (oops_in_progress)
    {
        locked = spin_trylock_irqsave(&jag_lock, flags);
    }
    else
    {
        spin_lock_irqsave(&jag_lock, flags);
    }

    jag_puts(s, count);

    if (locked)
    {
        spin_unlock_irqrestore(&jag_lock, flags);
    }
}

static struct tty_driver *jag_vidcon_device(struct console *co, int *index)
{
    *index = 0;
    return jag_tty_driver;
}

static struct console jag_vidcon = {
    .name = "ttyV",
    .write = jag_vidcon_write,
    .device = jag_vidcon_device,
    .flags = CON_ENABLED | CON_PRINTBUFFER,
    .index = -1,
};

static void __init jag_mklink(unsigned long addr, u32 *hi, u32 *lo)
{
    *hi = addr >> 11;
    *lo = (u32)(addr & 0xFFFF) << 21;
}

static void __init jag_build_list(void)
{
    unsigned long stop = (unsigned long)&jag_olist[OL_STOP];
    unsigned long text = (unsigned long)&jag_olist[OL_TEXT];
    u32 txt_hi, txt_lo;
    u16 ypos_b, ypos_t, xpos;

    jag_mklink(stop, &jag_link_hi, &jag_link_lo);
    jag_mklink(text, &txt_hi, &txt_lo);

    jag_olist[0] = jag_link_hi;
    jag_olist[1] = BRANCHOBJ | O_BRLT | jag_link_lo | ((u32)a_vde << 3);
    jag_olist[2] = jag_link_hi;
    jag_olist[3] = BRANCHOBJ | O_BRGT | jag_link_lo | ((u32)a_vdb << 3);

    ypos_b = (a_vdb + 2) & 0xFFFE;
    ypos_t = ypos_b + 2 * BANNER_H + 4;
    xpos = (disp_width / 4 - FB_WIDTH) / 2;

    jag_ban_hi = txt_hi | ((u32)(unsigned long)jag_banner << 8);
    jag_ban_lo = txt_lo | ((u32)BANNER_H << 14) | ((u32)ypos_b << 3);
    jag_olist[OL_BANNER] = jag_ban_hi;
    jag_olist[OL_BANNER + 1] = jag_ban_lo;

    jag_olist[OL_BANNER + 2] = FB_PHRASES >> 4;
    jag_olist[OL_BANNER + 3] = O_DEPTH8 | O_NOGAP | xpos | ((u32)FB_PHRASES << 18) | ((u32)FB_PHRASES << 28);
    jag_bmp_lo = jag_link_lo | ((u32)FB_HEIGHT << 14) | ((u32)ypos_t << 3);
    jag_update_bmp_pointer();
    jag_olist[OL_TEXT] = jag_bmp_hi;
    jag_olist[OL_TEXT + 1] = jag_bmp_lo;
    jag_olist[OL_TEXT + 2] = FB_PHRASES >> 4;
    jag_olist[OL_TEXT + 3] = O_DEPTH8 | O_NOGAP | xpos | ((u32)FB_PHRASES << 18) | ((u32)FB_PHRASES << 28);

    jag_olist[OL_STOP] = 0;
    jag_olist[OL_STOP + 1] = STOPOBJ | O_STOPINTS;
}

void __init jaguar_video_console_init(void)
{
    unsigned long list = (unsigned long)jag_olist;
    u16 w, h, hmid, vmid, hdb, hde;

    VI = 0xFFFF;

    if (JOY_CONFIG & VIDTYPE)
    {
        w = NTSC_WIDTH;
        hmid = NTSC_HMID;
        h = NTSC_HEIGHT;
        vmid = NTSC_VMID;
    }
    else
    {
        w = PAL_WIDTH;
        hmid = PAL_HMID;
        h = PAL_HEIGHT;
        vmid = PAL_VMID;
    }

    disp_width = w;
    disp_height = h;

    hde = (w / 2 - 1) | 0x400;
    hdb = hmid - w / 2 + 4;
    HDE = hde;
    HDB1 = hdb;
    HDB2 = hdb;

    a_vdb = vmid - h;
    a_vde = vmid + h;
    VDB = a_vdb;
    VDE = 0xFFFF;

    BORD1 = 0;
    BORD2 = 0;
    BG = 0;

    CLUT(0) = 0x0000;
    CLUT(1) = 0xFFFF;

    memset(jag_fb, 0, sizeof(jag_fb));
    memset(jag_banner, 0, sizeof(jag_banner));
    jag_draw_banner();
    jag_build_list();

    OLPLO = (u16)list;
    OLPHI = (u16)(list >> 16);

    if (!jaguar_gpu_refresher_start())
    {
        VI = a_vde | 1;
        jaguar_int1_enable |= C_VIDENA;
        INT1 = jaguar_int1_enable;
    }

    VMODE = VMODE_VAL;

    jag_video_up = true;

    register_console(&jag_vidcon);
}
