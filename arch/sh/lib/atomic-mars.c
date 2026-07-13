// SPDX-License-Identifier: GPL-2.0
/*
 * Heavily inspired/based on arch/sparc/lib/atomic32.c
 */

#include <linux/kernel.h>
#include <linux/irqflags.h>
#include <linux/export.h>
#include <linux/smp.h>
#include <asm/mars-peterson.h>

#define MARS_ATOMIC_HASH_SHIFT 4
#define MARS_ATOMIC_HASH_SIZE (1 << MARS_ATOMIC_HASH_SHIFT)
#define MARS_ATOMIC_HASH(ptr) (&__mars_atomic_locks[(((unsigned long)(ptr)) >> 2) & (MARS_ATOMIC_HASH_SIZE - 1)])

static struct mars_plock __mars_atomic_locks[MARS_ATOMIC_HASH_SIZE] = {
	[0 ... MARS_ATOMIC_HASH_SIZE - 1] = __MARS_PLOCK_INIT
};

u32 __mars_cmpxchg_u32(volatile u32 *ptr, u32 old, u32 new)
{
	struct mars_plock *l = MARS_ATOMIC_HASH(ptr);
	unsigned long flags;
	unsigned int cpu;
	u32 prev;

	local_irq_save(flags);
	cpu = hard_smp_processor_id();
	mars_plock_lock(l, cpu);

	prev = *ptr;

	if (prev == old) {
		*ptr = new;
		mars_store_flush((volatile unsigned char *)ptr);
	}

	mars_plock_unlock(l, cpu);
	local_irq_restore(flags);

	return prev;
}
EXPORT_SYMBOL(__mars_cmpxchg_u32);

u32 __mars_xchg_u32(volatile u32 *ptr, u32 new)
{
	struct mars_plock *l = MARS_ATOMIC_HASH(ptr);
	unsigned long flags;
	unsigned int cpu;
	u32 prev;

	local_irq_save(flags);
	cpu = hard_smp_processor_id();
	mars_plock_lock(l, cpu);

	prev = *ptr;
	*ptr = new;
	mars_store_flush((volatile unsigned char *)ptr);

	mars_plock_unlock(l, cpu);
	local_irq_restore(flags);

	return prev;
}
EXPORT_SYMBOL(__mars_xchg_u32);

u32 __mars_cmpxchg_small(volatile void *ptr, u32 old, u32 new, int size)
{
	unsigned long addr = (unsigned long)ptr;
	volatile u32 *base = (volatile u32 *)(addr & ~3UL);
	struct mars_plock *l = MARS_ATOMIC_HASH(base);
	unsigned long flags;
	unsigned int cpu;
	u32 prev;

	local_irq_save(flags);
	cpu = hard_smp_processor_id();
	mars_plock_lock(l, cpu);

	if (size == 1) {
		volatile u8 *p = ptr;
		prev = *p;
		if (prev == (u8)old) {
			*p = (u8)new;
		}
	} else {
		volatile u16 *p = ptr;
		prev = *p;
		if (prev == (u16)old)
			*p = (u16)new;
	}
	mars_store_flush((volatile unsigned char *)ptr);

	mars_plock_unlock(l, cpu);
	local_irq_restore(flags);

	return prev;
}
EXPORT_SYMBOL(__mars_cmpxchg_small);