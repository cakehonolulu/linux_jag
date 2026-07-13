// SPDX-License-Identifier: GPL-2.0
/*
 * SH7604 FRT
 */

#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/cpuhotplug.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/percpu.h>
#include <linux/sched_clock.h>
#include <linux/smp.h>

#define FRT_TIER 0x0
#define FRT_FTCSR 0x1
#define FRT_FRC_H 0x2
#define FRT_FRC_L 0x3
#define FRT_OCR_H 0x4
#define FRT_OCR_L 0x5
#define FRT_TCR 0x6
#define FRT_TOCR 0x7

#define TIER_OCIAE BIT(3)
#define TIER_RESERVED 0x01

#define FTCSR_OCFA BIT(3)

#define TCR_CKS_DIV8 0x00
#define TOCR_SELECT_OCRA 0x00

// percpu
#define SH7604_IPRB	((void __iomem *)0xfffffe60)
#define SH7604_VCRC	((void __iomem *)0xfffffe66)
#define IPRB_FRT_SHIFT	8
#define IPRB_FRT_MASK (0xf << IPRB_FRT_SHIFT)
#define VCRC_FOC_MASK 0x007f

#define FRT_IRQ_LEVEL 5

struct sh7604_frt {
	struct clock_event_device ced;
	u16 ocra;
	u16 period;
	bool periodic;
};

#ifdef CONFIG_SH_MARS_SMP
void mars_secondary_init_irq(void);
#else
static inline void mars_secondary_init_irq(void) { }
#endif

static DEFINE_PER_CPU(struct sh7604_frt, sh7604_frt_percpu);

static void __iomem *frt_base;
static u32 frt_rate;
static int frt_irq;
static irq_hw_number_t frt_vector;

static inline u8 frt_read(unsigned int reg)
{
	return __raw_readb(frt_base + reg);
}

static inline void frt_write(unsigned int reg, u8 val)
{
	__raw_writeb(val, frt_base + reg);
}

static u16 frt_read_frc(void)
{
	u8 hi = frt_read(FRT_FRC_H);
	u8 lo = frt_read(FRT_FRC_L);

	return ((u16)hi << 8) | lo;
}

static void frt_write_ocra(u16 val)
{
	frt_write(FRT_OCR_H, val >> 8);
	frt_write(FRT_OCR_L, val & 0xff);
}

static struct sh7604_frt *ced_to_frt(struct clock_event_device *ced)
{
	return container_of(ced, struct sh7604_frt, ced);
}

static void frt_arm(struct sh7604_frt *frt, u16 compare)
{
	frt->ocra = compare;
	frt_write_ocra(compare);
	frt_write(FRT_FTCSR, frt_read(FRT_FTCSR) & ~FTCSR_OCFA);
	frt_write(FRT_TIER, TIER_RESERVED | TIER_OCIAE);
}

static void frt_disarm(void)
{
	frt_write(FRT_TIER, TIER_RESERVED);
}

static irqreturn_t sh7604_frt_interrupt(int irq, void *dev_id)
{
	struct sh7604_frt *frt = this_cpu_ptr(&sh7604_frt_percpu);

	frt_write(FRT_FTCSR, frt_read(FRT_FTCSR) & ~FTCSR_OCFA);

	if (frt->periodic) {
		frt->ocra += frt->period;
		frt_write_ocra(frt->ocra);
	} else {
		frt_disarm();
	}

	frt->ced.event_handler(&frt->ced);

	return IRQ_HANDLED;
}

static int sh7604_frt_set_next_event(unsigned long delta,
				     struct clock_event_device *ced)
{
	frt_arm(ced_to_frt(ced), frt_read_frc() + delta);

	return 0;
}

static int sh7604_frt_state_shutdown(struct clock_event_device *ced)
{
	ced_to_frt(ced)->periodic = false;
	frt_disarm();

	return 0;
}

static int sh7604_frt_state_oneshot(struct clock_event_device *ced)
{
	return sh7604_frt_state_shutdown(ced);
}

static int sh7604_frt_state_periodic(struct clock_event_device *ced)
{
	struct sh7604_frt *frt = ced_to_frt(ced);

	frt->periodic = true;
	frt->period = DIV_ROUND_CLOSEST(frt_rate, HZ);
	frt_arm(frt, frt_read_frc() + frt->period);

	return 0;
}

static u64 sh7604_frt_cs_read(struct clocksource *cs)
{
	return frt_read_frc();
}

static struct clocksource sh7604_frt_cs = {
	.name = "sh7604-frt",
	.rating	= 200,
	.read = sh7604_frt_cs_read,
	.mask = CLOCKSOURCE_MASK(16),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

static u64 notrace sh7604_frt_sched_clock_read(void)
{
	return frt_read_frc();
}

static int sh7604_frt_starting_cpu(unsigned int cpu)
{
	struct sh7604_frt *frt = this_cpu_ptr(&sh7604_frt_percpu);

	frt_write(FRT_TIER, TIER_RESERVED);
	frt_write(FRT_TCR, TCR_CKS_DIV8);
	frt_write(FRT_TOCR, TOCR_SELECT_OCRA);
	frt_write(FRT_FTCSR, 0x00);

	__raw_writew((__raw_readw(SH7604_VCRC) & ~VCRC_FOC_MASK) | (frt_vector & VCRC_FOC_MASK), SH7604_VCRC);
	__raw_writew((__raw_readw(SH7604_IPRB) & ~IPRB_FRT_MASK) | (FRT_IRQ_LEVEL << IPRB_FRT_SHIFT), SH7604_IPRB);

	if (cpu) mars_secondary_init_irq();

	frt->ced.name = "sh7604-frt";
	frt->ced.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT;
	frt->ced.rating = 200;
	frt->ced.irq = frt_irq;
	frt->ced.cpumask = cpumask_of(cpu);
	frt->ced.set_next_event = sh7604_frt_set_next_event;
	frt->ced.set_state_shutdown = sh7604_frt_state_shutdown;
	frt->ced.set_state_oneshot = sh7604_frt_state_oneshot;
	frt->ced.set_state_periodic = sh7604_frt_state_periodic;

	clockevents_config_and_register(&frt->ced, frt_rate, 100, 0xffff);

	return 0;
}

static int sh7604_frt_dying_cpu(unsigned int cpu)
{
	frt_disarm();

	return 0;
}

static int __init sh7604_frt_init(struct device_node *np)
{
	u32 freq;
	int ret;

	frt_base = of_iomap(np, 0);

	if (!frt_base) {
		pr_err("%pOF: unable to map FRT registers\n", np);
		return -ENXIO;
	}

	if (of_property_read_u32(np, "clock-frequency", &freq)) {
		pr_err("%pOF: missing clock-frequency\n", np);
		return -EINVAL;
	}

	frt_irq = irq_of_parse_and_map(np, 0);
	if (frt_irq <= 0) {
		pr_err("%pOF: unable to map interrupt\n", np);
		return -EINVAL;
	}

	frt_vector = irqd_to_hwirq(irq_get_irq_data(frt_irq));
	frt_rate = freq / 8;

	ret = request_irq(frt_irq, sh7604_frt_interrupt, IRQF_TIMER | IRQF_IRQPOLL | IRQF_PERCPU, "sh7604-frt", NULL);
	if (ret) {
		pr_err("%pOF: request_irq failed: %d\n", np, ret);
		return ret;
	}

	ret = clocksource_register_hz(&sh7604_frt_cs, frt_rate);
	if (ret)
		return ret;

	sched_clock_register(sh7604_frt_sched_clock_read, 16, frt_rate);

	// smp/cpu hotplug
	ret = cpuhp_setup_state(CPUHP_AP_MARS_STARTING,
				"clockevents/mars/frt:starting",
				sh7604_frt_starting_cpu, sh7604_frt_dying_cpu);
	if (ret < 0) {
		pr_err("%pOF: cpuhp_setup_state failed: %d\n", np, ret);
		return ret;
	}

	pr_info("%pOF: SH7604 FRT at %u Hz, vector %lu, level %u (per-CPU)\n", np, frt_rate, (unsigned long)frt_vector, FRT_IRQ_LEVEL);

	return 0;
}

TIMER_OF_DECLARE(sh7604_frt, "renesas,sh7604-frt", sh7604_frt_init);
