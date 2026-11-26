// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

#include <asm/ktsan.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/slab.h>

#include "../slab.h"

/*
 * Returns the start of the memory block that @addr belongs to: the slab
 * object for slab memory, the (head) page otherwise. Sync objects are
 * attached to that block and die with it.
 *
 * Ported from the reference KTSAN, with SLAB's s_mem/reciprocal_divide()
 * object lookup replaced by SLUB's nearest_obj().
 */
uptr_t kt_memblock_addr(uptr_t addr)
{
	struct page *page;
	struct slab *slab;

	if (!ktsan_virt_addr_valid((void *)addr))
		return 0;
	page = virt_to_head_page((void *)addr);

	/*
	 * If the page is a slab page, we want to delete sync objects when a
	 * slab object is freed.
	 */
	slab = page_slab(page);
	if (slab)
		return (uptr_t)nearest_obj(slab->slab_cache, slab,
					   (void *)addr);

	return (uptr_t)page_address(page);
}

/*
 * memblock_tab is full: every one of its KT_MAX_MEMBLOCK_COUNT entries is a
 * live block (slab object or page) that holds at least one sync object, e.g.
 * a dentry with its d_lock. Disable KTSAN (KTSAN_WARN_ON) rather than BUG().
 * Printed deferred, like the reports (see report.c): this can run under a
 * console's lock.
 */
static void kt_memblock_tab_full(void)
{
	static bool reported;
	kt_stats_t stat;

	if (xchg(&reported, true))
		return;

	printk_deferred_enter();
	KTSAN_WARN_ON(true);
	pr_err("KTSAN: limit on number of memory blocks with sync objects (%d) is reached, KTSAN is disabled\n",
	       KT_MAX_MEMBLOCK_COUNT);
	if (IS_ENABLED(CONFIG_KTSAN_STATS)) {
		kt_stat_collect(&stat);
		pr_err("KTSAN: memblock objects %lu (alloc %lu, free %lu), sync objects %lu (alloc %lu, free %lu)\n",
		       stat.stat[kt_stat_memblock_objects],
		       stat.stat[kt_stat_memblock_alloc],
		       stat.stat[kt_stat_memblock_free],
		       stat.stat[kt_stat_sync_objects],
		       stat.stat[kt_stat_sync_alloc],
		       stat.stat[kt_stat_sync_free]);
	}
	printk_deferred_exit();
}

/* Returns the block locked, or NULL when memblock_tab is full. */
static kt_tab_memblock_t *kt_memblock_ensure_created(kt_thr_t *thr, uptr_t addr)
{
	kt_tab_memblock_t *memblock;
	bool created;

	memblock = kt_tab_access(&kt_ctx.memblock_tab, addr, &created, false);
	if (unlikely(!memblock)) {
		kt_memblock_tab_full();
		return NULL;
	}

	if (created) {
		INIT_LIST_HEAD(&memblock->sync_list);
		INIT_LIST_HEAD(&memblock->lock_list);

		kt_stat_inc(kt_stat_memblock_objects);
		kt_stat_inc(kt_stat_memblock_alloc);
	}

	return memblock;
}

void kt_memblock_add_sync(kt_thr_t *thr, uptr_t addr, kt_tab_sync_t *sync)
{
	kt_tab_memblock_t *memblock;

	memblock = kt_memblock_ensure_created(thr, addr);
	/*
	 * Table full, KTSAN is now off. The sync object stays unattached, like
	 * one in vmalloc memory: it is never dropped.
	 */
	if (!memblock)
		return;
	list_add(&sync->list, &memblock->sync_list);
	kt_spin_unlock(&memblock->tab.lock);
}

void kt_memblock_remove_sync(kt_thr_t *thr, uptr_t addr, kt_tab_sync_t *sync)
{
	kt_tab_memblock_t *memblock;
	struct list_head *entry, *tmp;
	bool deleted = false;

	memblock = kt_tab_access(&kt_ctx.memblock_tab, addr, NULL, false);
	BUG_ON(memblock == NULL);

	list_for_each_safe(entry, tmp, &memblock->sync_list) {
		if (list_entry(entry, kt_tab_sync_t, list) == sync) {
			list_del_init(entry);
			deleted = true;
			break;
		}
	}

	BUG_ON(!deleted);
	kt_spin_unlock(&memblock->tab.lock);
}

/*
 * A new block is stamped as written by the allocating thread, so a later
 * access from a thread that has no happens-before with the allocation is
 * reported as racing with it.
 */
void kt_memblock_alloc(kt_thr_t *thr, uptr_t pc, uptr_t addr, size_t size,
		       bool write_to_shadow)
{
	/*
	 * Memory block size is multiple of KT_GRAIN, so round the size up.
	 * In the "worst" case we will catch OOB accesses due to this.
	 */
	size = round_up(size, KT_GRAIN);

	if (write_to_shadow)
		kt_access_range_imitate(thr, pc, addr, size, false);
}

/*
 * Freeing a block is a write to all of it, which catches races between the
 * free and accesses from other threads. Sync objects inside the block are
 * dropped, so that a lock or atomic later allocated at the same address
 * doesn't inherit their clocks.
 */
void kt_memblock_free(kt_thr_t *thr, uptr_t pc, uptr_t addr, size_t size,
		      bool write_to_shadow)
{
	kt_tab_memblock_t *memblock;
	struct list_head *entry, *tmp;
	kt_tab_sync_t *sync;

	if (write_to_shadow)
		kt_access_range(thr, pc, addr, size, false);

	memblock = kt_tab_access(&kt_ctx.memblock_tab, addr, NULL, true);

	if (memblock == NULL)
		return;

	list_for_each_safe(entry, tmp, &memblock->sync_list) {
		sync = list_entry(entry, kt_tab_sync_t, list);
		list_del_init(entry);
		kt_sync_free(thr, sync->tab.key);
	}

	kt_spin_unlock(&memblock->tab.lock);
	kt_cache_free(&kt_ctx.memblock_tab.obj_cache, memblock);

	kt_stat_dec(kt_stat_memblock_objects);
	kt_stat_inc(kt_stat_memblock_free);
}

/*
 * kt_memblock_addr() keys the sync objects of a compound page by its head.
 * When the page is split, each of them moves to the new head of the part
 * that contains it, so that it dies with that part and not with the old head.
 * They are taken off the old block first, since adding them to another block
 * while holding the lock of this one could deadlock.
 */
void ktsan_split_folio(struct page *page, unsigned int old_order,
		       unsigned int new_order)
{
	uptr_t head = (uptr_t)page_address(page);
	uptr_t new_size = PAGE_SIZE << new_order;
	kt_tab_memblock_t *memblock;
	kt_tab_sync_t *sync, *tmp;
	LIST_HEAD(moved);

	if (new_order >= old_order)
		return;

	ENTER(KT_ENTER_NORMAL);
	memblock = kt_tab_access(&kt_ctx.memblock_tab, head, NULL, false);
	if (memblock) {
		list_for_each_entry_safe(sync, tmp, &memblock->sync_list, list) {
			if (sync->tab.key - head >= new_size)
				list_move(&sync->list, &moved);
		}
		kt_spin_unlock(&memblock->tab.lock);
	}

	list_for_each_entry_safe(sync, tmp, &moved, list) {
		list_del_init(&sync->list);
		kt_memblock_add_sync(thr,
				     head + round_down(sync->tab.key - head,
						       new_size),
				     sync);
	}
	LEAVE();
}

void ktsan_slab_alloc(struct kmem_cache *s, void *object)
{
	/*
	 * Objects of SLAB_TYPESAFE_BY_RCU caches may legitimately be accessed
	 * after they were freed and reused, so their shadow is left alone.
	 */
	if (!object || (s->flags & SLAB_TYPESAFE_BY_RCU))
		return;

	ENTER(KT_ENTER_NORMAL);
	kt_memblock_alloc(thr, pc, (uptr_t)object, s->object_size, true);
	LEAVE();
}

void ktsan_slab_free(struct kmem_cache *s, void *object, bool still_accessible)
{
	ENTER(KT_ENTER_NORMAL);
	kt_memblock_free(thr, pc, (uptr_t)object, s->object_size,
			 !still_accessible);
	LEAVE();
}
