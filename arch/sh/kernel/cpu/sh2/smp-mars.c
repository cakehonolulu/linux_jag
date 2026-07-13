// SPDX-License-Identifier: GPL-2.0

#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <asm/cmpxchg.h>
#include <asm/smp.h>
#include <linux/cpuhotplug.h>

// softfloat so we basically reuse the hw dvsr register (percpu) to act as physical ident. of the core
#define MARS_CPUID_REG ((void __iomem *)0xffffff00)

#define MARS_INT_MASK ((void __iomem *)0x20004000)
#define MARS_CMDINT_CLR	((void __iomem *)0x2000401a)
#define MARS_INTMASK_CMD BIT(1)

#define MARS_COMM_CMD(cpu) ((void __iomem *)(0x20004020 + 4 * (cpu)))
#define MARS_COMM_PARAM(cpu) ((void __iomem *)(0x20004022 + 4 * (cpu)))
#define MARS_CMD_IPI 0x0b00

#define MARS_SLAVE_MBOX_PC ((void __iomem *)0x2603fff0)

#define MARS_CMD_VECTOR 68

DEFINE_PER_CPU(unsigned, mars_ipi_messages);

extern u32 *sh2_cpuid_addr;

extern void mars_comm0_xact(u16 cmd, u16 param);

static void mars_68k_cmd(unsigned int cpu, u16 cmd, u16 param)
{
	unsigned long flags;

	if (cpu == 0) {
		mars_comm0_xact(cmd, param);
		return;
	}

	local_irq_save(flags);
	__raw_writew(param, MARS_COMM_PARAM(cpu));
	__raw_writew(cmd, MARS_COMM_CMD(cpu));

	while (__raw_readw(MARS_COMM_CMD(cpu)) != 0) {
		cpu_relax();
	}

	local_irq_restore(flags);
}

static irqreturn_t mars_ipi_interrupt_handler(int irq, void *arg)
{
	unsigned int cpu = hard_smp_processor_id();
	volatile unsigned *pmsg = &per_cpu(mars_ipi_messages, cpu);
	unsigned int messages, i;

	do {
		messages = xchg(pmsg, 0);
		__raw_writew(0, MARS_CMDINT_CLR);
	} while (0);

	messages |= xchg(pmsg, 0);

	if (!messages) {
		return IRQ_HANDLED;
	}

	for (i = 0; i < SMP_MSG_NR; i++) {
		if (messages & (1U << i)) {
			smp_message_recv(i);
        }
	}

	// not sure how correct?
	return IRQ_HANDLED;
}

static void mars_smp_setup(void)
{
	__raw_writel(0, MARS_CPUID_REG);
	sh2_cpuid_addr = (u32 *)MARS_CPUID_REG;
}

void mars_secondary_init_irq(void)
{
    u16 mask = __raw_readw(MARS_INT_MASK);
    __raw_writew(mask | MARS_INTMASK_CMD, MARS_INT_MASK);
}

static void mars_prepare_cpus(unsigned int max_cpus)
{
	unsigned int i;
	u16 mask;

	if (request_irq(MARS_CMD_VECTOR, mars_ipi_interrupt_handler, IRQF_PERCPU, "ipi", mars_ipi_interrupt_handler)) {
		pr_err("MARS SMP: cannot request CMD IPI irq\n");
		max_cpus = 1;
	}

	mask = __raw_readw(MARS_INT_MASK);
	__raw_writew(mask | MARS_INTMASK_CMD, MARS_INT_MASK);

	for (i = max_cpus; i < NR_CPUS; i++) {
		set_cpu_possible(i, false);
		set_cpu_present(i, false);
	}
}

static void mars_start_cpu(unsigned int cpu, unsigned long entry_point)
{
	if (!cpu) return;

	__raw_writel(entry_point, MARS_SLAVE_MBOX_PC);

	pr_info("MARS SMP: released secondary SH2 to %08lx\n", entry_point);
}

static unsigned int mars_smp_processor_id(void)
{
    unsigned int val = __raw_readl(MARS_CPUID_REG);
	return val;
}

static void mars_send_ipi(unsigned int cpu, unsigned int message)
{
	volatile unsigned *pmsg = &per_cpu(mars_ipi_messages, cpu);
	unsigned int old;

	do {
		old = *pmsg;
	} while (cmpxchg(pmsg, old, old | (1U << message)) != old);

	mars_68k_cmd(hard_smp_processor_id(), MARS_CMD_IPI | cpu, 0);
}

static struct plat_smp_ops mars_smp_ops = {
	.smp_setup = mars_smp_setup,
	.prepare_cpus = mars_prepare_cpus,
	.start_cpu = mars_start_cpu,
	.smp_processor_id = mars_smp_processor_id,
	.send_ipi = mars_send_ipi,
	.cpu_die = native_cpu_die,
	.cpu_disable = native_cpu_disable,
	.play_dead = native_play_dead,
};

CPU_METHOD_OF_DECLARE(mars_cpu_method, "sega,mars-spin-table", &mars_smp_ops);
