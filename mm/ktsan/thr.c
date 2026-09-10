// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

//#include <linux/atomic.h>
//#include <linux/kernel.h>
//#include <linux/list.h>
//#include <linux/sched.h>
//#include <linux/spinlock.h>

__init void kt_thr_pool_init(void)
{
	kt_thr_pool_t *pool = &kt_ctx.thr_pool;

	kt_cache_init(&pool->cache, sizeof(kt_thr_t), KT_MAX_THREAD_COUNT);
	memset(pool->thrs, 0, sizeof(pool->thrs));
	pool->new_id = 0;
	pool->new_pid = 0;
	INIT_LIST_HEAD(&pool->quarantine);
	pool->quarantine_size = 0;
	kt_spin_init(&pool->lock);
}

