// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/mm.h>

/*
 * Sync objects carry happens-before between threads: a release joins the
 * thread's clock into the object's clock, an acquire joins the object's
 * clock into the thread's. One object exists per address used for
 * synchronization (a lock, an atomic, a hand-placed ktsan_sync_*() key).
 */

/*
 * Whether @addr lies inside the struct page array. Atomics on struct page
 * fields (_refcount, flags bitops, ...) run on every page, and a sync object
 * per page would exhaust sync_tab, so atomics skip these addresses.
 *
 * The reference checked NODE_DATA(0)'s span, which only covers node 0.
 * With SPARSEMEM_VMEMMAP all struct pages are one virtual array.
 */
bool kt_sync_is_page_struct_addr(uptr_t addr)
{
#ifdef CONFIG_SPARSEMEM_VMEMMAP
	return addr >= (uptr_t)vmemmap &&
	       addr < (uptr_t)(vmemmap + max_pfn);
#else
	return false;
#endif
}

/* Unique ids are handed out to each CPU in batches. */
static u64 kt_sync_new_uid(kt_thr_t *thr)
{
	const u64 batch = 64;
	kt_cpu_t *cpu = thr->cpu;

	if (cpu->sync_uid_pos == cpu->sync_uid_end) {
		cpu->sync_uid_pos = kt_atomic64_fetch_add_no_ktsan(
			&kt_ctx.sync_uid_gen, batch);
		cpu->sync_uid_end = cpu->sync_uid_pos + batch;
	}
	return cpu->sync_uid_pos++;
}

/*
 * Returns the sync object for @addr, creating it on first use, or NULL when
 * sync_tab is full (KTSAN is then disabled). The object is returned locked;
 * the caller unlocks sync->tab.lock.
 */
kt_tab_sync_t *kt_sync_ensure_created(kt_thr_t *thr, uptr_t pc, uptr_t addr)
{
	kt_tab_sync_t *sync;
	uptr_t memblock_addr;
	bool created;

	sync = kt_tab_access(&kt_ctx.sync_tab, addr, &created, false);
	if (unlikely(sync == NULL)) {
		/* Deferred, as in report.c: this can run under a console's lock. */
		printk_deferred_enter();
		if (KTSAN_WARN_ON(true))
			pr_err("KTSAN: limit on number of sync objects (%d) is reached, KTSAN is disabled\n",
			       KT_MAX_SYNC_COUNT);
		printk_deferred_exit();
		return NULL;
	}

	if (created) {
		kt_clk_init(&sync->clk);
		sync->lock_tid = -1;
		INIT_LIST_HEAD(&sync->list);
		sync->pc = pc;
		sync->last_lock_time = 0;
		sync->last_unlock_time = 0;
		sync->uid = kt_sync_new_uid(thr);

		/*
		 * Tie the object to the block that contains it, so it dies
		 * when the block is freed. Addresses outside the direct map
		 * and kernel image (vmalloc, modules) have no block, and
		 * their objects are never dropped.
		 */
		memblock_addr = kt_memblock_addr(addr);
		if (memblock_addr)
			kt_memblock_add_sync(thr, memblock_addr, sync);

		kt_stat_inc(kt_stat_sync_objects);
		kt_stat_inc(kt_stat_sync_alloc);
	}

	return sync;
}

/* Removes sync object from hash table and frees it. */
void kt_sync_free(kt_thr_t *thr, uptr_t addr)
{
	kt_tab_sync_t *sync;

	sync = kt_tab_access(&kt_ctx.sync_tab, addr, NULL, true);
	BUG_ON(sync == NULL);

	kt_spin_unlock(&sync->tab.lock);
	kt_cache_free(&kt_ctx.sync_tab.obj_cache, sync);

	kt_stat_dec(kt_stat_sync_objects);
	kt_stat_inc(kt_stat_sync_free);
}

/* thr.clk = max(thr.clk, sync.clk). The caller holds sync->tab.lock. */
void kt_acquire(kt_thr_t *thr, uptr_t pc, kt_tab_sync_t *sync)
{
	kt_stat_inc(kt_stat_acquire);
	kt_clk_acquire(&thr->clk, &sync->clk);
}

/*
 * sync.clk = max(sync.clk, thr.clk). The caller holds sync->tab.lock.
 * No tick is needed afterwards: every access ticks the thread's own clock
 * first, so accesses after the release get epochs above the released one.
 */
void kt_release(kt_thr_t *thr, uptr_t pc, kt_tab_sync_t *sync)
{
	kt_stat_inc(kt_stat_release);
	kt_clk_acquire(&sync->clk, &thr->clk);
}

void kt_sync_acquire(kt_thr_t *thr, uptr_t pc, uptr_t addr)
{
	kt_tab_sync_t *sync;

#ifdef CONFIG_KTSAN_DEBUG
	kt_trace_add_event(thr, kt_event_acquire, kt_compress(pc));
#endif

	sync = kt_sync_ensure_created(thr, pc, addr);
	if (sync == NULL)
		return;
	kt_acquire(thr, pc, sync);
	kt_spin_unlock(&sync->tab.lock);
}

void kt_sync_release(kt_thr_t *thr, uptr_t pc, uptr_t addr)
{
	kt_tab_sync_t *sync;

#ifdef CONFIG_KTSAN_DEBUG
	kt_trace_add_event(thr, kt_event_release, kt_compress(pc));
#endif

	sync = kt_sync_ensure_created(thr, pc, addr);
	if (sync == NULL)
		return;
	kt_release(thr, pc, sync);
	kt_spin_unlock(&sync->tab.lock);
}

/*
 * Hand-placed happens-before edges, for synchronization KTSAN can't see
 * (e.g. through the uninstrumented scheduler): everything before
 * ktsan_sync_release(addr) happens before everything after a later
 * ktsan_sync_acquire(addr).
 */
void ktsan_sync_acquire(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_sync_acquire(thr, pc, (uptr_t)addr);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_sync_acquire);

void ktsan_sync_release(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_sync_release(thr, pc, (uptr_t)addr);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_sync_release);
