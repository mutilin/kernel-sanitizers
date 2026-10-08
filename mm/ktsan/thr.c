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

/*
 * Creates a thread for a new task. @thr is the creating (parent) thread, or
 * NULL for thread #0. Returns NULL if all KT_MAX_THREAD_COUNT threads are
 * alive; the task then stays untracked instead of BUG()ing like the reference.
 */
kt_thr_t *kt_thr_create(kt_thr_t *thr, int pid)
{
	kt_thr_pool_t *pool = &kt_ctx.thr_pool;
	kt_thr_t *new;
	int i;

	kt_spin_lock(&pool->lock);
	new = kt_cache_alloc(&pool->cache);
	if (new) {
		new->id = pool->new_id;
		pool->new_id++;
		pool->thrs[new->id] = new;
	} else if (pool->quarantine_size) {
		/* Reuse a dead thread together with its id. */
		new = list_first_entry(&pool->quarantine, kt_thr_t,
				       quarantine_list);
		list_del(&new->quarantine_list);
		pool->quarantine_size--;
	} else {
		kt_spin_unlock(&pool->lock);
		pr_warn_once("KTSAN: maximum number of threads (%d) is reached, new tasks are not tracked\n",
			     KT_MAX_THREAD_COUNT);
		return NULL;
	}
	/*
	 * Kernel does not assign PIDs to some threads, give them fake
	 * negative PIDs so that they are distinguishable in reports.
	 */
	if (pid == 0)
		pid = --pool->new_pid;
	kt_spin_unlock(&pool->lock);

	new->pid = pid;
	new->inside = 0;
	new->cpu = NULL;
	kt_clk_init(&new->clk);
	kt_clk_init(&new->acquire_clk);
	new->acquire_active = 0;
	kt_clk_init(&new->release_clk);
	new->release_active = 0;
	kt_stack_init(&new->stack);
	new->mutexset.size = 0;
	kt_trace_init(&new->trace);
	new->read_disable_depth = 0;
	new->event_disable_depth = 0;
	new->report_disable_depth = 0;
	new->preempt_disable_depth = 0;
	new->irqs_disabled = false;
	new->irq_flags_before_disable = 0;
	INIT_LIST_HEAD(&new->quarantine_list);
	INIT_LIST_HEAD(&new->percpu_list);
	for (i = 0; i < ARRAY_SIZE(new->seqcount); i++) {
		new->seqcount[i] = 0;
		new->seqcount_pc[i] = 0;
	}
	new->seqcount_ignore = 0;
	new->interrupt_depth = 0;
#ifdef CONFIG_KTSAN_DEBUG
	new->last_event_disable_time = 0;
	new->last_event_enable_time = 0;
#endif

	kt_stat_inc(kt_stat_thread_create);
	kt_stat_inc(kt_stat_threads);

	/* thr == NULL when thread #0 is being initialized. */
	if (thr == NULL)
		return new;

	/* Thread creation is a happens-before edge from the parent. */
	kt_clk_acquire(&new->clk, &thr->clk);

	return new;
}

/* Puts the thread of a dead task into quarantine for later reuse. */
void kt_thr_destroy(kt_thr_t *thr, kt_thr_t *old)
{
	kt_thr_pool_t *pool = &kt_ctx.thr_pool;
	int i;

	KT_BUG_ON(old->event_disable_depth != 0);
	for (i = 0; i < ARRAY_SIZE(old->seqcount); i++)
		KT_BUG_ON(old->seqcount[i] != 0);
	KT_BUG_ON(old->read_disable_depth != 0);
	KT_BUG_ON(old->seqcount_ignore != 0);
	KT_BUG_ON(old->interrupt_depth != 0);
	KT_BUG_ON(old->cpu != NULL);

	kt_spin_lock(&pool->lock);
	list_add_tail(&old->quarantine_list, &pool->quarantine);
	pool->quarantine_size++;
	kt_spin_unlock(&pool->lock);

	kt_stat_inc(kt_stat_thread_destroy);
	kt_stat_dec(kt_stat_threads);
}

kt_thr_t *kt_thr_get(int id)
{
	kt_thr_pool_t *pool = &kt_ctx.thr_pool;
	void *thr;

	KT_BUG_ON(id < 0);
	KT_BUG_ON(id >= KT_MAX_THREAD_COUNT);
	kt_spin_lock(&pool->lock);
	thr = pool->thrs[id];
	kt_spin_unlock(&pool->lock);

	return thr;
}

/* Binds @thr to the current CPU. */
void kt_thr_start(kt_thr_t *thr, uptr_t pc)
{
	/* TODO: kt_trace_add_event(kt_event_thr_start) once trace.c is ported. */
	thr->cpu = this_cpu_ptr(kt_ctx.cpus);
	KT_BUG_ON(thr->cpu->thr != NULL);
	thr->cpu->thr = thr;
}

/* Unbinds @thr from its CPU. */
void kt_thr_stop(kt_thr_t *thr, uptr_t pc)
{
	KT_BUG_ON(thr->event_disable_depth != 0);
	/*
	 * TODO: kt_percpu_release() and kt_trace_add_event(kt_event_thr_stop)
	 * once sync_percpu.c and trace.c are ported.
	 */
	KT_BUG_ON(thr->cpu == NULL);
	KT_BUG_ON(thr->cpu->thr != thr);
	thr->cpu->thr = NULL;
	thr->cpu = NULL;
}

