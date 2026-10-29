// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

#include <linux/kernel.h>
#include <linux/printk.h>
#include <asm/barrier.h>

static kt_spinlock_t kt_report_lock;

static unsigned long racy_pc[32768];
static unsigned int nracy_pc;

static void kt_print_mutexset(kt_mutexset_t *set)
{
	kt_locked_mutex_t *mtx;
	kt_stack_t *stack;
	int i;

	for (i = 0; i < set->size; i++) {
		mtx = &set->mtx[i];
		pr_err("Mutex %llu is %slocked here:\n", mtx->uid,
		       mtx->write ? "" : "read ");
		stack = kt_stack_depot_get(&kt_ctx.stack_depot, mtx->stack);
		if (stack)
			kt_stack_print(stack, 0);
	}
}

static void print_mop(bool new, bool wr, bool atomic, uptr_t addr, int sz,
		      int pid, int cpu)
{
	pr_err("%s at 0x%px of size %d by thread %d on CPU %d:\n",
	       new ? (atomic ? (wr ? "Atomic write" : "Atomic read") :
			       (wr ? "Write" : "Read")) :
		     (atomic ? (wr ? "Previous atomic write" :
				     "Previous atomic read") :
			       (wr ? "Previous write" : "Previous read")),
	       (void *)addr, sz, pid, cpu);
}

void kt_report_race(kt_thr_t *new, kt_race_info_t *info)
{
	int i, n;
	kt_thr_t *old;
	uptr_t new_pc, old_pc;
	kt_trace_state_t old_state;

	kt_stat_inc(kt_stat_races);

	if (new->report_disable_depth != 0)
		return;

	new_pc = kt_decompress(kt_trace_last_data(new));

	old = kt_thr_get(info->old.tid);
	if (old == NULL)
		return;
	kt_trace_restore_state(old, info->old.clock, &old_state);
	if (old_state.stack.size == 0)
		return;
	old_pc = kt_decompress(old_state.stack.pc[old_state.stack.size - 1]);

	n = smp_load_acquire(&nracy_pc);
	for (i = 0; i < n; i += 2) {
		if (new_pc == racy_pc[i] && old_pc == racy_pc[i + 1])
			return;
	}

	kt_spin_lock(&kt_report_lock);

	if (nracy_pc < ARRAY_SIZE(racy_pc)) {
		racy_pc[nracy_pc] = new_pc;
		racy_pc[nracy_pc + 1] = old_pc;
		smp_store_release(&nracy_pc, nracy_pc + 2);
	}

	pr_err("==================================================================\n");
	pr_err("ThreadSanitizer: data-race in %ps\n\n", (void *)new_pc);

	print_mop(true, !info->new.read, info->new.atomic, info->addr,
		  (1 << info->new.size), new->pid, smp_processor_id());
	kt_stack_print(&new->stack, new_pc);

	print_mop(false, !info->old.read, info->old.atomic, info->addr,
		  (1 << info->old.size), old_state.pid, old_state.cpu_id);
	kt_stack_print(&old_state.stack, 0);

	if (new->mutexset.size) {
		pr_err("Mutexes locked by thread %d:\n", new->pid);
		kt_print_mutexset(&new->mutexset);
	}

	if (old_state.mutexset.size) {
		pr_err("Mutexes locked by thread %d:\n", old_state.pid);
		kt_print_mutexset(&old_state.mutexset);
	}

#ifdef CONFIG_KTSAN_DEBUG
	pr_err("Thread %d clock: {T%d: %lu, T%d: %lu}\n", new->pid, new->pid,
	       (unsigned long)kt_clk_get(&new->clk, new->id), old->pid,
	       (unsigned long)kt_clk_get(&new->clk, old->id));
	pr_err("Thread %d clock: {T%d: %lu}\n", old->pid, old->pid,
	       (unsigned long)info->old.clock);
#endif
	pr_err("==================================================================\n");

	kt_stat_inc(kt_stat_reports);

	kt_spin_unlock(&kt_report_lock);
}
