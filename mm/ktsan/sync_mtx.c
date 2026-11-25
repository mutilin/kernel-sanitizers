// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

#include <linux/export.h>

/*
 * Locks. A lock is the sync object keyed by its address: unlock releases the
 * thread's clock into it, lock acquires it, so everything done under one hold
 * of the lock happens before everything done under the next one.
 *
 * Shared (read) holds use the same clock as exclusive ones. That also orders
 * readers against each other, which hides a race between two readers that
 * both write under a read lock (TODO: a separate read clock, as in user-space
 * TSan).
 *
 * sync->lock_tid is the exclusive owner, -1 when the lock is free or only read
 * held. It goes wrong when KTSAN missed a lock or an unlock: one that happened
 * while ENTER() dropped events, the drop of a SLAB_TYPESAFE_BY_RCU object's
 * sync objects while the lock in it is still held, a raw arch_spin_*() call.
 * Those are holes in the hooks, not runtime bugs, so they don't BUG(): the
 * state is fixed up, and with CONFIG_KTSAN_DEBUG the first few are printed.
 */

#ifdef CONFIG_KTSAN_DEBUG
static void kt_mtx_mismatch(kt_thr_t *thr, uptr_t pc, kt_tab_sync_t *sync,
			    const char *what)
{
	static int printed;

	if (printed >= 16)
		return;
	printed++;
	pr_err("KTSAN: %s of lock %px at %pS by thread %d (owner %d)\n", what,
	       (void *)sync->tab.key, (void *)pc, thr->pid, sync->lock_tid);
}
#else
static inline void kt_mtx_mismatch(kt_thr_t *thr, uptr_t pc,
				   kt_tab_sync_t *sync, const char *what)
{
}
#endif

void kt_mtx_post_lock(kt_thr_t *thr, uptr_t pc, uptr_t addr, bool wr,
		      bool try, bool success)
{
	kt_tab_sync_t *sync;

	/*
	 * A failed trylock, or a sleeping lock that gave up (signal, timeout):
	 * the thread doesn't hold the lock, so there is no edge.
	 */
	if (!success)
		return;

	sync = kt_sync_ensure_created(thr, pc, addr);
	if (!sync)
		return;

	/* Trace event and mutexset entry, for reports. */
	kt_mutex_lock(thr, pc, sync->uid, wr);

	kt_acquire(thr, pc, sync);

	/*
	 * A 1-byte read of the lock stands in for the lock op. It races with
	 * an initialization of the lock that doesn't happen before this lock,
	 * i.e. an unsafe publication.
	 *
	 * It comes after the acquire: the previous owner's writes to the lock
	 * happen before us. And it is atomic: arch_spin_unlock() is visible to
	 * the instrumentation (queued_spin_unlock() is an smp_store_release(),
	 * an atomic write), and it lands after the owner's release, so a plain
	 * read would race with it on every handoff. Atomic vs. atomic never
	 * races; the plain writes of an init or a free still do.
	 */
	kt_access(thr, pc, addr, KT_ACCESS_SIZE_1, true, true);

	if (sync->lock_tid != -1) {
		/* KTSAN missed the previous owner's unlock. */
		kt_mtx_mismatch(thr, pc, sync, "lock of a held lock");
		sync->lock_tid = -1;
	}
	if (wr)
		sync->lock_tid = thr->id;
	/* Lets a bad-unlock report replay the trace to where we locked. */
	sync->last_lock_time = kt_clk_get(&thr->clk, thr->id);

	kt_spin_unlock(&sync->tab.lock);
}

void kt_mtx_pre_unlock(kt_thr_t *thr, uptr_t pc, uptr_t addr, bool wr)
{
	kt_tab_sync_t *sync;

	sync = kt_sync_ensure_created(thr, pc, addr);
	if (!sync)
		return;

	/*
	 * Same atomic stand-in read as in kt_mtx_post_lock(). Here it races
	 * with a free of the memory that holds the lock (a free counts as a
	 * plain write).
	 */
	kt_access(thr, pc, addr, KT_ACCESS_SIZE_1, true, true);

	kt_mutex_unlock(thr, sync->uid, wr);

	kt_release(thr, pc, sync);

	if (wr) {
		if (sync->lock_tid == -1)
			kt_mtx_mismatch(thr, pc, sync, "unlock of a free lock");
		else if (sync->lock_tid != thr->id)
			kt_report_bad_mtx_unlock(thr, pc, sync);
		sync->lock_tid = -1;
	} else if (sync->lock_tid != -1) {
		kt_mtx_mismatch(thr, pc, sync, "read unlock of a write held lock");
		sync->lock_tid = -1;
	}
	sync->last_unlock_time = kt_clk_get(&thr->clk, thr->id);

	kt_spin_unlock(&sync->tab.lock);
}

/*
 * The lock stays held across a downgrade, so there is no clock op. Only
 * exclusive ownership ends.
 */
void kt_mtx_downgrade(kt_thr_t *thr, uptr_t pc, uptr_t addr)
{
	kt_tab_sync_t *sync;

	sync = kt_sync_ensure_created(thr, pc, addr);
	if (!sync)
		return;

	kt_mutex_downgrade(thr, sync->uid);

	if (sync->lock_tid == -1)
		kt_mtx_mismatch(thr, pc, sync, "downgrade of a free lock");
	else if (sync->lock_tid != thr->id)
		kt_report_bad_mtx_unlock(thr, pc, sync);
	sync->lock_tid = -1;

	kt_spin_unlock(&sync->tab.lock);
}

/*
 * Placeholders for deadlock detection. They skip ENTER()/LEAVE(), which would
 * only cost an irq save/restore on every lock op.
 */
void ktsan_mtx_pre_lock(void *addr, bool write, bool trylock)
{
}
EXPORT_SYMBOL(ktsan_mtx_pre_lock);

void ktsan_mtx_post_unlock(void *addr, bool write)
{
}
EXPORT_SYMBOL(ktsan_mtx_post_unlock);

void ktsan_mtx_post_lock(void *addr, bool write, bool trylock, bool success)
{
	ENTER(KT_ENTER_NORMAL);
	kt_mtx_post_lock(thr, pc, (uptr_t)addr, write, trylock, success);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_mtx_post_lock);

void ktsan_mtx_pre_unlock(void *addr, bool write)
{
	ENTER(KT_ENTER_NORMAL);
	kt_mtx_pre_unlock(thr, pc, (uptr_t)addr, write);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_mtx_pre_unlock);

void ktsan_mtx_downgrade(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_mtx_downgrade(thr, pc, (uptr_t)addr);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_mtx_downgrade);
