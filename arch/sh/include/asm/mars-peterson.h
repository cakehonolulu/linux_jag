/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_SH_MARS_PETERSON_H
#define __ASM_SH_MARS_PETERSON_H

#include <linux/compiler.h>

struct mars_plock {
	volatile unsigned char flag[2];
	volatile unsigned char turn;
	unsigned char __pad;
};

#define __MARS_PLOCK_INIT { { 0, 0 }, 0, 0 }

static inline void mars_store_flush(const volatile unsigned char *p)
{
	(void)READ_ONCE(*p);
}

static inline void mars_plock_lock(struct mars_plock *l, unsigned int cpu)
{
	unsigned int other = cpu ^ 1;

	WRITE_ONCE(l->flag[cpu], 1);
	WRITE_ONCE(l->turn, other);
	mars_store_flush(&l->turn);

	while (READ_ONCE(l->flag[other]) && READ_ONCE(l->turn) == other) {
		cpu_relax();
	}
}

static inline void mars_plock_unlock(struct mars_plock *l, unsigned int cpu)
{
	WRITE_ONCE(l->flag[cpu], 0);
	mars_store_flush(&l->flag[cpu]);
}

#endif /* __ASM_SH_MARS_PETERSON_H */
