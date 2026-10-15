// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

/*
 * TODO: kt_sync_ensure_created(), kt_sync_acquire()/release() and
 * kt_acquire()/release() from the reference, once there are sync hooks
 * (locks, atomics) to drive them.
 */

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
