// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

#include <asm/barrier.h>
#include <linux/atomic.h>
#include <linux/processor.h>

void kt_spin_init(kt_spinlock_t *l)
{
	WRITE_ONCE(l->state, 0);
}
 
void kt_spin_lock(kt_spinlock_t *l)
{
	for (;;) {
		if (xchg(&l->state, 1) == 0)
			return;
		while (READ_ONCE(l->state) != 0)
			cpu_relax();
	}
}
 
void kt_spin_unlock(kt_spinlock_t *l)
{
	smp_store_release(&l->state, 0);
}
 
int kt_spin_is_locked(kt_spinlock_t *l)
{
	return READ_ONCE(l->state) != 0;
}
