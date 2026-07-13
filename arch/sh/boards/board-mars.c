// SPDX-License-Identifier: GPL-2.0

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/serial_core.h>
#include <linux/of.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/module.h>
#include <linux/cpuhotplug.h>
#include <linux/spinlock.h>
#include <linux/string.h>

// Location of the m68k dispatcher(s)
#define MARS_COMM0 ((void __iomem *)0x20004020)
#define MARS_COMM1 ((void __iomem *)0x20004022)
#define MARS_CMD_PUTC   0x0800

static DEFINE_RAW_SPINLOCK(mars_comm0_lock);

void mars_comm0_xact(u16 cmd, u16 param)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&mars_comm0_lock, flags);
	__raw_writew(param, MARS_COMM1);
	__raw_writew(cmd, MARS_COMM0);
	
	while (__raw_readw(MARS_COMM0) != 0) {
		cpu_relax();
	}

	raw_spin_unlock_irqrestore(&mars_comm0_lock, flags);
}

static void mars_putc(int c)
{
    mars_comm0_xact(0x0800, c);
}

static void mars_console_write(struct console *con, const char *s, unsigned int n)
{
    while (n--) {
        if (*s == '\n')
            mars_putc('\r');
        mars_putc(*s++);
    }
}

static int __init mars_early_console_setup(struct earlycon_device *dev, const char *opt)
{
    dev->con->write = mars_console_write;
    return 0;
}

EARLYCON_DECLARE(mars_comm, mars_early_console_setup);
OF_EARLYCON_DECLARE(mars_comm, "sega,mars-comm", mars_early_console_setup);

static struct console mars_boot_console = {
    .name   = "marsboot",
    .write  = mars_console_write,
    .flags  = CON_PRINTBUFFER | CON_BOOT | CON_ANYTIME,
    .index  = -1,
};

void __init mars_boot_console_init(void)
{
    register_console(&mars_boot_console);
}

static struct tty_driver *mars_tty_driver;

static int mars_tty_open(struct tty_struct *tty, struct file *filp)
{
    return 0;
}

static void mars_tty_close(struct tty_struct *tty, struct file *filp)
{
}

static unsigned int mars_tty_write_room(struct tty_struct *tty)
{
    return 256;
}

static void marsfb_mirror(const u8 *s, size_t n);

static ssize_t mars_tty_write(struct tty_struct *tty, const u8 *buf, size_t count)
{
    size_t i;

    marsfb_mirror(buf, count);

    for (i = 0; i < count; i++) {
        if (buf[i] == '\n') {
            mars_putc('\r');
		}

        mars_putc(buf[i]);
    }
    return count;
}

static const struct tty_operations mars_tty_ops = {
    .open = mars_tty_open,
    .close = mars_tty_close,
    .write = mars_tty_write,
    .write_room = mars_tty_write_room,
};

static struct tty_driver *mars_console_device(struct console *c, int *index)
{
    *index = c->index;
    return mars_tty_driver;
}

static struct console mars_console = {
    .name = "ttyMARS",
    .write = mars_console_write,
    .device = mars_console_device,
    .flags = CON_PRINTBUFFER | CON_ENABLED,
    .index = 0,
};

static struct tty_port mars_tty_port;
static const struct tty_port_operations mars_port_ops = { };

static int __init mars_tty_init(void)
{
    mars_tty_driver = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW);

    if (IS_ERR(mars_tty_driver)) {
        return PTR_ERR(mars_tty_driver);
	}

    mars_tty_driver->driver_name = "mars_tty";
    mars_tty_driver->name = "ttyMARS";
    mars_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
    mars_tty_driver->subtype = SERIAL_TYPE_NORMAL;
    mars_tty_driver->init_termios= tty_std_termios;

    tty_set_operations(mars_tty_driver, &mars_tty_ops);

	tty_port_init(&mars_tty_port);
	mars_tty_port.ops = &mars_port_ops;
	tty_port_link_device(&mars_tty_port, mars_tty_driver, 0);

    if (tty_register_driver(mars_tty_driver)) {
        tty_driver_kref_put(mars_tty_driver);
        return -ENODEV;
    }

    register_console(&mars_console);

    return 0;
}

device_initcall(mars_tty_init);


#define MARS_SYS_INTMSK	((void __iomem *)0x20004000)
#define MARS_VDP_MODE ((void __iomem *)0x20004100)
#define MARS_VDP_FBCR ((void __iomem *)0x2000410a)
#define MARS_CRAM ((void __iomem *)0x20004200)
#define MARS_FB ((void __iomem *)0x24000000)

#define MARS_FM BIT(15)

#define VDP_MODE_PP	0x0001 //SHould be 8pp
#define VDP_PRIO_32X 0x0080 //32x layer ontop of vdp's

#define FBCR_FS	BIT(0)

#define FB_W 320
#define FB_H 224
#define GLYPH_W 8
#define GLYPH_H 8
#define TXT_COLS (FB_W / GLYPH_W)
#define TXT_ROWS (FB_H / GLYPH_H)

// palette indices (white on black)
#define COLOR_BG 0
#define COLOR_FG 1

#define PAL_BG 0x0000
#define PAL_FG 0x7fff

#include "font.h"

#define FONT_GLYPHS	(sizeof(textfont) / GLYPH_H)

static u8 marsfb_text[TXT_ROWS][TXT_COLS];
static u32 marsfb_rowgen[TXT_ROWS];
static u32 marsfb_bankgen[2][TXT_ROWS];
static unsigned int marsfb_tbase;
static u8 marsfb_banktbase[2];
static unsigned int marsfb_x, marsfb_y;
static u16 marsfb_fs;
static bool marsfb_swap_pending;
static DEFINE_RAW_SPINLOCK(marsfb_lock);

static inline void marsfb_fbw(unsigned int wordoff, u16 val)
{
	__raw_writew(val, MARS_FB + 2 * wordoff);
}

static void marsfb_wait_flip(void)
{
	unsigned long t;

	if (!marsfb_swap_pending)
		return;

	// probably wronng but couldn't find demos for this
	for (t = 0; t < 4000000UL; t++) {
		if ((__raw_readw(MARS_VDP_FBCR) & FBCR_FS) == marsfb_fs) {
			marsfb_swap_pending = false;
			return;
		}
		cpu_relax();
	}

	marsfb_swap_pending = false;
}

static void marsfb_write_table(void)
{
	unsigned int s;

	for (s = 0; s < 256; s++) {
		marsfb_fbw(s, 0x100 + ((marsfb_tbase * GLYPH_H + s) % FB_H) * (FB_W / 2));
	}
}

static void marsfb_render_row(unsigned int row)
{
	unsigned int col, gy, px;

	for (col = 0; col < TXT_COLS; col++) {
		u8 c = marsfb_text[row][col];
		const u8 *glyph;

		if (c >= FONT_GLYPHS) {
			c = '?';
		}

		glyph = textfont + c * GLYPH_H;
		unsigned int base = 0x100 + (row * GLYPH_H) * (FB_W / 2) + col * (GLYPH_W / 2);

		for (gy = 0; gy < GLYPH_H; gy++) {
			unsigned int o = base + gy * (FB_W / 2);
			u8 bits = glyph[gy];

			for (px = 0; px < GLYPH_W / 2; px++) {
				u16 w;
				w = ((bits & 0x80) ? COLOR_FG : COLOR_BG) << 8;
				w |= (bits & 0x40) ? COLOR_FG : COLOR_BG;
				bits <<= 2;
				marsfb_fbw(o + px, w);
			}
		}
	}
}

static void marsfb_sync_bank(void)
{
	unsigned int bank = marsfb_fs & 1;
	unsigned int row;
	bool dirty = false;

	if (marsfb_banktbase[bank] != marsfb_tbase) {
		marsfb_write_table();
		marsfb_banktbase[bank] = marsfb_tbase;
		dirty = true;
	}

	for (row = 0; row < TXT_ROWS; row++) {
		if (marsfb_bankgen[bank][row] != marsfb_rowgen[row]) {
			marsfb_render_row(row);
			marsfb_bankgen[bank][row] = marsfb_rowgen[row];
			dirty = true;
		}
	}

	if (dirty) {
		marsfb_fs ^= 1;
		__raw_writew(marsfb_fs, MARS_VDP_FBCR);
		marsfb_swap_pending = true;
	}
}

static void marsfb_scroll(void)
{
	unsigned int recycled = marsfb_tbase;

	marsfb_tbase = (marsfb_tbase + 1) % TXT_ROWS;
	memset(marsfb_text[recycled], ' ', TXT_COLS);
	marsfb_rowgen[recycled]++;
}

static void marsfb_putc(char c)
{
	switch (c) {
		case '\r':
			marsfb_x = 0;
			return;
		case '\n':
			marsfb_x = 0;
			marsfb_y++;
			break;
		case '\t':
			marsfb_x = (marsfb_x + 8) & ~7u;
			break;
		default: {
			unsigned int ring = (marsfb_tbase + marsfb_y) % TXT_ROWS;

			if ((u8)c < 0x20) {
				return;
			}

			marsfb_text[ring][marsfb_x] = c;
			marsfb_rowgen[ring]++;
			marsfb_x++;
			break;
		}
	}

	if (marsfb_x >= TXT_COLS) {
		marsfb_x = 0;
		marsfb_y++;
	}

	while (marsfb_y >= TXT_ROWS) {
		marsfb_scroll();
		marsfb_y--;
	}
}

static void marsfb_console_write(struct console *con, const char *s, unsigned int n)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&marsfb_lock, flags);
	marsfb_wait_flip();

	while (n--) {
		marsfb_putc(*s++);
	}

	marsfb_sync_bank();
	raw_spin_unlock_irqrestore(&marsfb_lock, flags);
}

static bool marsfb_ready;

static void marsfb_mirror(const u8 *s, size_t n)
{
	if (marsfb_ready) {
		marsfb_console_write(NULL, (const char *)s, n);
	}
}

static void marsfb_init_bank(void)
{
	unsigned int i;
	marsfb_write_table();
	
	for (i = 0; i < (FB_W / 2) * FB_H; i++) {
		marsfb_fbw(0x100 + i, 0);
	}
}

static struct console marsfb_console = {
	.name = "marsfb",
	.write = marsfb_console_write,
	.flags = CON_PRINTBUFFER | CON_ENABLED | CON_ANYTIME,
	.index = -1,
};

static int __init marsfb_console_init(void)
{
	unsigned int i;

	if (!(__raw_readw(MARS_SYS_INTMSK) & MARS_FM)) {
		return -ENODEV;
	}

	BUILD_BUG_ON(sizeof(textfont) % GLYPH_H);
	BUILD_BUG_ON(FONT_GLYPHS < 0x80);

	__raw_writew(0, MARS_VDP_MODE);
	__raw_writew(PAL_BG, MARS_CRAM + 2 * COLOR_BG);
	__raw_writew(PAL_FG, MARS_CRAM + 2 * COLOR_FG);

	marsfb_fs = __raw_readw(MARS_VDP_FBCR) & FBCR_FS;
	marsfb_init_bank();
	marsfb_fs ^= 1;
	__raw_writew(marsfb_fs, MARS_VDP_FBCR);
	marsfb_wait_flip();
	marsfb_init_bank();

	marsfb_banktbase[0] = 0;
	marsfb_banktbase[1] = 0;

	memset(marsfb_text, ' ', sizeof(marsfb_text));
	for (i = 0; i < TXT_ROWS; i++) {
		marsfb_rowgen[i] = 1;
		marsfb_bankgen[0][i] = 0;
		marsfb_bankgen[1][i] = 0;
	}

	__raw_writew(VDP_PRIO_32X | VDP_MODE_PP, MARS_VDP_MODE);

	marsfb_ready = true;
	register_console(&marsfb_console);
	return 0;
}
console_initcall(marsfb_console_init);
