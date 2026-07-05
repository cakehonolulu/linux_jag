#include <asm/atomic.h>
#include <asm/machdep.h>
#include <asm/page.h>
#include <asm/setup.h>
#include <linux/console.h>
#include <linux/init.h>

#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#define TOM_BASE 0xF00000
#define PIT0 (*(volatile unsigned short *)(TOM_BASE + 0x50))
#define PIT1 (*(volatile unsigned short *)(TOM_BASE + 0x52))
#define INT1 (*(volatile unsigned short *)(TOM_BASE + 0xE0))
#define INT2 (*(volatile unsigned short *)(TOM_BASE + 0xE2))

#define INT1_VIDENA 0x0001
#define INT1_PITENA 0x0008

#define JAGUAR_TOM_CLOCK 26590906UL

#define JERRY_BASE 0xF10000
#define ASI_DATA (*(volatile unsigned short *)(JERRY_BASE + 0x30))
#define ASI_CTRL (*(volatile unsigned short *)(JERRY_BASE + 0x32))
#define ASI_CLK (*(volatile unsigned short *)(JERRY_BASE + 0x34))

unsigned short jaguar_int1_enable;

static void jaguar_cons_write(struct console *co, const char *str, unsigned int count)
{
    unsigned int i;
    for (i = 0; i < count; i++)
    {
        if (str[i] == '\n')
        {
            while ((ASI_CTRL & 0x0100) == 0)
            {
                barrier();
            }

            ASI_DATA = '\r';
        }

        while ((ASI_CTRL & 0x0100) == 0)
        {
            barrier();
        }

        ASI_DATA = str[i];
    }
}

static struct console jaguar_early_console = {
    .name = "early_jerry",
    .write = jaguar_cons_write,
    .flags = CON_PRINTBUFFER | CON_BOOT,
    .index = -1,
};

static void __init jaguar_early_console_init(void)
{
    ASI_CLK = 86;
    ASI_CTRL = 0;

    register_console(&jaguar_early_console);
}

extern void jaguar_uart_poll_rx(void);
extern void jaguar_video_vblank(void);
extern void __init jaguar_video_console_init(void);

static irqreturn_t jaguar_video_interrupt(int irq, void *dev_id)
{
    jaguar_video_vblank();
    return IRQ_HANDLED;
}

static irqreturn_t jaguar_timer_interrupt(int irq, void *dev_id)
{
    legacy_timer_tick(1);
    jaguar_uart_poll_rx();
    return IRQ_HANDLED;
}

void __init jaguar_sched_init(void)
{
    unsigned long ticks = JAGUAR_TOM_CLOCK / HZ;
    unsigned int pit0 = 1, pit1;

    while ((ticks / (pit0 + 1)) > 65536 && pit0 < 65535)
    {
        pit0++;
    }

    pit1 = ticks / (pit0 + 1);

    if (pit1)
    {
        pit1--;
    }

    PIT0 = pit0;
    PIT1 = pit1;

    if (request_irq(2, jaguar_timer_interrupt, IRQF_TIMER, "timer", NULL))
    {
        pr_err("Couldn't register timer interrupt\n");
    }
}

void __init config_BSP(char *command_line, int len)
{
    jaguar_early_console_init();
    jaguar_video_console_init();

    // Use TOM as console, basically
    strscpy(command_line, "console=ttyV0 init=/init", len);

    // Probably redundant?
    memblock_reserve(0x0, 0x400);

    /*
        Since Linux isn't loaded on 0x0 (We XIP) we need to memcpy the vectors
        to RAM base before any system call/irq/whatever triggers. This is to
        basically comply with the 68000 way of doing things (VBR and all that).
    */
    extern unsigned long e_vectors;
    memcpy((void *)0x0, &e_vectors, 0x400);

    mach_sched_init = jaguar_sched_init;

    if (request_irq(1, jaguar_video_interrupt, 0, "video", NULL))
        pr_err("Couldn't register video interrupt\n");
}