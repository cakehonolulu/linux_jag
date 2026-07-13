/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_SH_CMPXCHG_MARS_H
#define __ASM_SH_CMPXCHG_MARS_H

extern u32 __mars_cmpxchg_u32(volatile u32 *p, u32 old, u32 new);
extern u32 __mars_xchg_u32(volatile u32 *p, u32 new);
extern u32 __mars_cmpxchg_small(volatile void *p, u32 old, u32 new, int size);

static inline unsigned long xchg_u32(volatile u32 *m, unsigned long val)
{
	return __mars_xchg_u32(m, val);
}

static inline unsigned long xchg_u16(volatile u16 *m, unsigned long val)
{
	u32 old;

	do {
		old = *m;
	} while (__mars_cmpxchg_small((volatile void *)m, old, val, 2) != old);

	return old;
}

static inline unsigned long xchg_u8(volatile u8 *m, unsigned long val)
{
	u32 old;

	do {
		old = *m;
	} while (__mars_cmpxchg_small((volatile void *)m, old, val, 1) != old);

	return old;
}

static inline unsigned long __cmpxchg_u32(volatile u32 *m,
					  unsigned long old, unsigned long new)
{
	return __mars_cmpxchg_u32(m, old, new);
}

#endif
