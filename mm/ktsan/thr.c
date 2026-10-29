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
	kt_time_t epoch = 0;
	bool reused = false;
	kt_thr_t *new;
	int i;

	kt_spin_lock(&pool->lock);
	new = kt_cache_alloc(&pool->cache);
	if (new) {
		new->id = pool->new_id;
		pool->new_id++;
		pool->thrs[new->id] = new;
	} else if (pool->quarantine_size > KT_THR_QUARANTINE_SIZE) {
		/* Reuse a dead thread together with its id. */
		new = list_first_entry(&pool->quarantine, kt_thr_t,
				       quarantine_list);
		list_del(&new->quarantine_list);
		pool->quarantine_size--;
		reused = true;
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

	if (reused)
		epoch = round_up(kt_clk_get(&new->clk, new->id) + 1,
				 KT_TRACE_PART_SIZE) - 1;

	new->pid = pid;
	new->inside = 0;
	new->cpu = NULL;
	kt_clk_init(&new->clk);
	kt_clk_init(&new->acquire_clk);
	new->acquire_active = 0;
	kt_clk_init(&new->release_clk);
	new->release_active = 0;
	kt_stack_init(&new->stack);
	new->stack_overflow = 0;
	kt_mutexset_init(&new->mutexset);
	if (!reused)
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

	new->clk.time[new->id] = epoch;

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
	kt_trace_add_event(thr, kt_event_thr_start,
			   smp_processor_id() | ((u64)(u32)thr->pid << 16));

	thr->cpu = this_cpu_ptr(kt_ctx.cpus);
	KT_BUG_ON(thr->cpu->thr != NULL);
	thr->cpu->thr = thr;
}

/* Unbinds @thr from its CPU. */
void kt_thr_stop(kt_thr_t *thr, uptr_t pc)
{
	KT_BUG_ON(thr->event_disable_depth != 0);
	/*
	 * TODO: kt_percpu_release() once sync_percpu.c is ported.
	 */
	kt_trace_add_event(thr, kt_event_thr_stop, smp_processor_id());

	KT_BUG_ON(thr->cpu == NULL);
	KT_BUG_ON(thr->cpu->thr != thr);
	thr->cpu->thr = NULL;
	thr->cpu = NULL;
}

/*
 * An interrupt runs as the interrupted thread, but must not inherit what that
 * thread is in the middle of (held locks, pending acquire/release, disabled
 * reads, ...). Stash that state in @state and start the interrupt clean.
 */
void kt_thr_interrupt(kt_thr_t *thr, uptr_t pc, kt_interrupted_t *state)
{
	KT_BUG_ON(state->thr != NULL);
	state->thr = thr;

	KT_BUG_ON(thr->event_disable_depth);

	kt_stack_copy(&state->stack, &thr->stack);
	kt_stack_init(&thr->stack);
	state->stack_overflow = thr->stack_overflow;
	thr->stack_overflow = 0;

	state->mutexset = thr->mutexset;
	kt_mutexset_init(&thr->mutexset);

	state->acquire_active = thr->acquire_active;
	if (thr->acquire_active) {
		thr->acquire_active = 0;
		state->acquire_clk = thr->acquire_clk;
	}
	state->release_active = thr->release_active;
	if (thr->release_active) {
		thr->release_active = 0;
		state->release_clk = thr->release_clk;
	}

	state->read_disable_depth = thr->read_disable_depth;
	thr->read_disable_depth = 0;
	state->report_disable_depth = thr->report_disable_depth;
	thr->report_disable_depth = 0;
	state->preempt_disable_depth = thr->preempt_disable_depth;
	thr->preempt_disable_depth = 0;

	list_replace_init(&thr->percpu_list, &state->percpu_list);

	memcpy(&state->seqcount, &thr->seqcount, sizeof(state->seqcount));
	memcpy(&state->seqcount_pc, &thr->seqcount_pc,
	       sizeof(state->seqcount_pc));
	memset(&thr->seqcount, 0, sizeof(thr->seqcount));
	memset(&thr->seqcount_pc, 0, sizeof(thr->seqcount_pc));
	state->seqcount_ignore = thr->seqcount_ignore;
	thr->seqcount_ignore = 0;

	kt_trace_add_event(thr, kt_event_interrupt, 0);
}

/* Returns to the interrupted thread the state saved by kt_thr_interrupt(). */
void kt_thr_resume(kt_thr_t *thr, uptr_t pc, kt_interrupted_t *state)
{
	int i;

	KT_BUG_ON(state->thr != thr);
	state->thr = NULL;

	KT_BUG_ON(thr->mutexset.size);
	KT_BUG_ON(thr->event_disable_depth);
	KT_BUG_ON(thr->read_disable_depth);
	KT_BUG_ON(thr->report_disable_depth);
	KT_BUG_ON(thr->preempt_disable_depth);
	KT_BUG_ON(thr->seqcount[0]);
	KT_BUG_ON(thr->seqcount_ignore);
	KT_BUG_ON(thr->stack.size || thr->stack_overflow);

	kt_trace_add_event(thr, kt_event_interrupt, 0);
	kt_stack_init(&thr->stack);
	for (i = 0; i < state->stack.size; i++)
		kt_func_entry(thr, kt_decompress(state->stack.pc[i]));
	thr->stack_overflow = state->stack_overflow;

	thr->mutexset = state->mutexset;
	for (i = 0; i < thr->mutexset.size; i++) {
		kt_locked_mutex_t *mtx = &thr->mutexset.mtx[i];

		kt_trace_add_event2(thr,
				    mtx->write ? kt_event_lock : kt_event_rlock,
				    mtx->uid, mtx->stack);
	}

	thr->acquire_active = state->acquire_active;
	if (thr->acquire_active)
		thr->acquire_clk = state->acquire_clk;
	thr->release_active = state->release_active;
	if (thr->release_active)
		thr->release_clk = state->release_clk;

	thr->read_disable_depth = state->read_disable_depth;
	thr->report_disable_depth = state->report_disable_depth;
	thr->preempt_disable_depth = state->preempt_disable_depth;

	/*
	 * TODO: kt_percpu_release() of what the interrupt acquired, once
	 * sync_percpu.c is ported. Until then the list stays empty.
	 */
	KT_BUG_ON(!list_empty(&thr->percpu_list));
	list_replace_init(&state->percpu_list, &thr->percpu_list);

	memcpy(&thr->seqcount, &state->seqcount, sizeof(thr->seqcount));
	memcpy(&thr->seqcount_pc, &state->seqcount_pc,
	       sizeof(thr->seqcount_pc));
	thr->seqcount_ignore = state->seqcount_ignore;
}

