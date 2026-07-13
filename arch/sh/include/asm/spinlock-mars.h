/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_SH_SPINLOCK_MARS_H
#define __ASM_SH_SPINLOCK_MARS_H

#include <asm/barrier.h>
#include <asm/processor.h>
#include <asm/cmpxchg.h>

#define arch_spin_is_locked(x) ((x)->lock <= 0)

static inline void arch_spin_lock(arch_spinlock_t *lock)
{
	while (!__mars_cmpxchg_u32(&lock->lock, 1, 0));
}

static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
	__mars_cmpxchg_u32(&lock->lock, 0, 1);
}

static inline int arch_spin_trylock(arch_spinlock_t *lock)
{
	return __mars_cmpxchg_u32(&lock->lock, 1, 0);
}

static inline void arch_read_lock(arch_rwlock_t *rw)
{
	unsigned old;
	do old = rw->lock;
	while (!old || __mars_cmpxchg_u32(&rw->lock, old, old-1) != old);
}

static inline void arch_read_unlock(arch_rwlock_t *rw)
{
	unsigned old;
	do old = rw->lock;
	while (__mars_cmpxchg_u32(&rw->lock, old, old+1) != old);
}

static inline void arch_write_lock(arch_rwlock_t *rw)
{
	while (__mars_cmpxchg_u32(&rw->lock, RW_LOCK_BIAS, 0) != RW_LOCK_BIAS);
}

static inline void arch_write_unlock(arch_rwlock_t *rw)
{
	__mars_cmpxchg_u32(&rw->lock, 0, RW_LOCK_BIAS);
}

static inline int arch_read_trylock(arch_rwlock_t *rw)
{
	unsigned old;
	do old = rw->lock;
	while (old && __mars_cmpxchg_u32(&rw->lock, old, old-1) != old);
	return !!old;
}

static inline int arch_write_trylock(arch_rwlock_t *rw)
{
	return __mars_cmpxchg_u32(&rw->lock, RW_LOCK_BIAS, 0) == RW_LOCK_BIAS;
}

#endif /* __ASM_SH_SPINLOCK_MARS_H */
