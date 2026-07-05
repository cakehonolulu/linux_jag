// SPDX-License-Identifier: GPL-2.0-only
/*
 * A simple (And probably very wrong) driver that repurposes
 * the Jerry DSP's TXD and RXD to behave as a more traditional
 * UART. Mainly used for bringup and earlycon prior to the Tom
 * display console. Based on the code of sifive.c kinda.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define JERRY_BASE 0xF10000
#define ASI_DATA (*(volatile unsigned short *)(JERRY_BASE + 0x30))
#define ASI_CTRL (*(volatile unsigned short *)(JERRY_BASE + 0x32))
#define ASI_CLK (*(volatile unsigned short *)(JERRY_BASE + 0x34))

#define JERRY_TX_READY 0x0100
#define JERRY_RX_READY 0x0080

static struct uart_port jaguar_uart_port;

static bool jaguar_port_ready;

static unsigned int jaguar_uart_tx_empty(struct uart_port *port)
{
    return (ASI_CTRL & JERRY_TX_READY) ? TIOCSER_TEMT : 0;
}

static void jaguar_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int jaguar_uart_get_mctrl(struct uart_port *port)
{
    return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}

static void jaguar_uart_stop_tx(struct uart_port *port)
{
}

static void jaguar_uart_start_tx(struct uart_port *port)
{
    unsigned char ch;

    while (uart_fifo_get(port, &ch))
    {
        while ((ASI_CTRL & JERRY_TX_READY) == 0)
            barrier();
        ASI_DATA = ch;
    }
}

static void jaguar_uart_stop_rx(struct uart_port *port)
{
}

static int jaguar_uart_startup(struct uart_port *port)
{
    return 0;
}

static void jaguar_uart_shutdown(struct uart_port *port)
{
}

static void jaguar_uart_set_termios(struct uart_port *port, struct ktermios *termios, const struct ktermios *old)
{
    ASI_CLK = 86; // Should be fine, real hw doesn't seem to complaiN?
    ASI_CTRL = 0;
}

static const char *jaguar_uart_type(struct uart_port *port)
{
    return "Jaguar Jerry UART";
}

static void jaguar_uart_release_port(struct uart_port *port)
{
}
static int jaguar_uart_request_port(struct uart_port *port)
{
    return 0;
}

static void jaguar_uart_config_port(struct uart_port *port, int flags)
{
    if (flags & UART_CONFIG_TYPE)
        port->type = PORT_UNKNOWN + 1;
}

static const struct uart_ops jaguar_uart_ops = {
    .tx_empty = jaguar_uart_tx_empty,
    .set_mctrl = jaguar_uart_set_mctrl,
    .get_mctrl = jaguar_uart_get_mctrl,
    .stop_tx = jaguar_uart_stop_tx,
    .start_tx = jaguar_uart_start_tx,
    .stop_rx = jaguar_uart_stop_rx,
    .startup = jaguar_uart_startup,
    .shutdown = jaguar_uart_shutdown,
    .set_termios = jaguar_uart_set_termios,
    .type = jaguar_uart_type,
    .release_port = jaguar_uart_release_port,
    .request_port = jaguar_uart_request_port,
    .config_port = jaguar_uart_config_port,
};

static void jaguar_console_putchar(struct uart_port *port, unsigned char ch)
{
    while ((ASI_CTRL & JERRY_TX_READY) == 0)
        barrier();
    ASI_DATA = ch;
}

static void jaguar_console_write(struct console *co, const char *s, unsigned int count)
{
    uart_console_write(&jaguar_uart_port, s, count, jaguar_console_putchar);
}

static int jaguar_console_setup(struct console *co, char *options)
{
    return 0;
}

static struct uart_driver jaguar_uart_driver;

static struct console jaguar_console = {
    .name = "ttyJ",
    .write = jaguar_console_write,
    .device = uart_console_device,
    .setup = jaguar_console_setup,
    .flags = CON_ENABLED | CON_PRINTBUFFER,
    .index = -1,
    .data = &jaguar_uart_driver,
};

static struct uart_driver jaguar_uart_driver = {
    .owner = THIS_MODULE,
    .driver_name = "jaguar_uart",
    .dev_name = "ttyJ",
    .major = TTY_MAJOR,
    .minor = 64,
    .nr = 1,
    .cons = &jaguar_console,
};

static struct uart_port jaguar_uart_port = {
    .ops = &jaguar_uart_ops,
    .iotype = UPIO_MEM,
    .mapbase = JERRY_BASE,
    .fifosize = 1,
    .flags = UPF_BOOT_AUTOCONF,
    .line = 0,
};

void jaguar_uart_poll_rx(void)
{
    if (ASI_CTRL & JERRY_RX_READY)
    {
        unsigned char ch = (unsigned char)ASI_DATA;

        if (jaguar_uart_port.state)
        {
            uart_insert_char(&jaguar_uart_port, 0, 0, ch, TTY_NORMAL);
            tty_flip_buffer_push(&jaguar_uart_port.state->port);
        }
    }
}
EXPORT_SYMBOL_GPL(jaguar_uart_poll_rx);

static int __init jaguar_console_init(void)
{
    register_console(&jaguar_console);

    return 0;
}
console_initcall(jaguar_console_init);

static struct platform_device *jaguar_uart_pdev;

static int __init jaguar_uart_init(void)
{
    int ret;

    jaguar_uart_pdev = platform_device_register_simple("jaguar_uart", 0, NULL, 0);
    if (IS_ERR(jaguar_uart_pdev))
    {
        ret = PTR_ERR(jaguar_uart_pdev);
        return ret;
    }
    jaguar_uart_port.dev = &jaguar_uart_pdev->dev;

    ret = uart_register_driver(&jaguar_uart_driver);

    if (ret)
    {
        return ret;
    }
    ret = uart_add_one_port(&jaguar_uart_driver, &jaguar_uart_port);
    if (ret)
    {
        uart_unregister_driver(&jaguar_uart_driver);
        return ret;
    }

    return 0;
}
device_initcall(jaguar_uart_init);
