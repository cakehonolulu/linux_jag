// SPDX-License-Identifier: GPL-2.0
/*
 * SH7604 onchip INTC (Sega 32X, maybe Saturn SH2?).
 */

#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>

#define SH7604_INTC_MIN_HWIRQ	64
#define SH7604_INTC_MAX_HWIRQ	127

static struct irq_chip sh7604_intc_chip;

static void sh7604_intc_noop(struct irq_data *data)
{
}

static int sh7604_intc_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &sh7604_intc_chip, handle_simple_irq);
	return 0;
}

static const struct irq_domain_ops sh7604_intc_domain_ops = {
	.map = sh7604_intc_map,
	.xlate = irq_domain_xlate_onecell,
};

static int __init sh7604_intc_of_init(struct device_node *node, struct device_node *parent)
{
	unsigned int min_irq = SH7604_INTC_MIN_HWIRQ;
	unsigned int dom_sz = SH7604_INTC_MAX_HWIRQ + 1;
	struct irq_domain *domain;
	int ret;

	pr_info("Initializing SH7604 INTC\n");

	sh7604_intc_chip.irq_mask = sh7604_intc_noop;
	sh7604_intc_chip.irq_unmask = sh7604_intc_noop;
	sh7604_intc_chip.name = "sh7604-intc";

	ret = irq_alloc_descs(-1, min_irq, dom_sz - min_irq, of_node_to_nid(node));

	if (ret < 0)
		return ret;

	domain = irq_domain_create_legacy(of_fwnode_handle(node), dom_sz - min_irq, min_irq, min_irq, &sh7604_intc_domain_ops, &sh7604_intc_chip);
	if (!domain)
		return -ENOMEM;

	return 0;
}

IRQCHIP_DECLARE(sh7604_intc, "renesas,sh7604-intc", sh7604_intc_of_init);
