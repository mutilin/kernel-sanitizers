// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

#include <linux/kernel.h>

static void kt_trace_follow(kt_trace_t *trace, unsigned long beg,
			    unsigned long end, kt_trace_state_t *state)
{
	kt_stack_handle_t stk;
	kt_event_t event;
	unsigned long i;

	for (i = beg; i <= end; i++) {
		event = trace->events[i];
		switch (event.type) {
		case kt_event_func_enter:
			if (state->stack.size + 1 < KT_MAX_STACK_FRAMES)
				kt_stack_push(&state->stack, event.data);
			break;
		case kt_event_func_exit:
			if (state->stack.size > 0)
				kt_stack_pop(&state->stack);
			break;
		case kt_event_thr_start:
			state->cpu_id = event.data & 0xffff;
			state->pid = (s32)(u32)(event.data >> 16);
			break;
		case kt_event_thr_stop:
			state->cpu_id = -1;
			break;
		case kt_event_lock:
		case kt_event_rlock:
			if (i == end)
				break;
			stk = *(u64 *)&trace->events[++i];
			if (state->mutexset.size < KT_MAX_LOCKED_MTX)
				kt_mutexset_lock(&state->mutexset, event.data,
						 stk, event.type == kt_event_lock);
			break;
		case kt_event_unlock:
			kt_mutexset_unlock(&state->mutexset, event.data, true);
			break;
		case kt_event_runlock:
			kt_mutexset_unlock(&state->mutexset, event.data, false);
			break;
		case kt_event_downgrade:
			kt_mutexset_downgrade(&state->mutexset, event.data);
			break;
		case kt_event_interrupt:
			kt_stack_init(&state->stack);
			kt_mutexset_init(&state->mutexset);
			break;
		default:
			break;
		}
	}
}

static bool kt_event_has_pc(kt_event_type_t type)
{
	switch (type) {
	case kt_event_mop:
#ifdef CONFIG_KTSAN_DEBUG
	case kt_event_acquire:
	case kt_event_release:
	case kt_event_nonmat_acquire:
	case kt_event_nonmat_release:
	case kt_event_membar_acquire:
	case kt_event_membar_release:
	case kt_event_preempt_enable:
	case kt_event_preempt_disable:
	case kt_event_irq_enable:
	case kt_event_irq_disable:
	case kt_event_event_disable:
	case kt_event_event_enable:
#endif
		return true;
	default:
		return false;
	}
}

void kt_trace_switch(kt_thr_t *thr)
{
	kt_trace_part_header_t *header;
	kt_trace_t *trace;
	kt_time_t clock;
	unsigned int part;

	trace = &thr->trace;
	clock = kt_clk_get(&thr->clk, thr->id);
	kt_spin_lock(&trace->lock);
	part = (clock % KT_TRACE_SIZE) / KT_TRACE_PART_SIZE;
	header = &trace->headers[part];
	header->state.stack = thr->stack;
	header->state.mutexset = thr->mutexset;
	header->state.pid = thr->pid;
	header->state.cpu_id = thr->cpu ? smp_processor_id() : -1;
	header->clock = clock;
	kt_spin_unlock(&trace->lock);
}

void kt_trace_add_event2(kt_thr_t *thr, kt_event_type_t type, u64 data,
			 u64 data2)
{
	kt_time_t clock;
	unsigned int pos;

	clock = kt_clk_get(&thr->clk, thr->id);
	if (((clock + 2) % KT_TRACE_PART_SIZE) == 0)
		kt_trace_add_event(thr, kt_event_nop, 0);
	kt_trace_add_event(thr, type, data);

	clock = kt_clk_get(&thr->clk, thr->id);
	pos = clock % KT_TRACE_SIZE + 1;
	BUG_ON((pos % KT_TRACE_PART_SIZE) == 0);
	BUILD_BUG_ON(sizeof(kt_event_t) != sizeof(data2));
	*(u64 *)&thr->trace.events[pos] = data2;
	kt_clk_tick(&thr->clk, thr->id);
}

void kt_trace_init(kt_trace_t *trace)
{
	int i;

	memset(trace, 0, sizeof(*trace));
	for (i = 0; i < KT_TRACE_PARTS; i++)
		trace->headers[i].state.cpu_id = -1;
	kt_spin_init(&trace->lock);
}

u64 kt_trace_last_data(kt_thr_t *thr)
{
	kt_time_t clock;
	kt_event_t event;

	clock = kt_clk_get(&thr->clk, thr->id);
	event = thr->trace.events[clock % KT_TRACE_SIZE];
	BUG_ON(event.type != kt_event_mop);
	return event.data;
}

void kt_trace_restore_state(kt_thr_t *thr, kt_time_t clock,
			    kt_trace_state_t *state)
{
	kt_trace_part_header_t *header;
	unsigned long beg, end;
	kt_trace_t *trace;
	kt_event_t event;
	unsigned int part;

	trace = &thr->trace;
	part = (clock % KT_TRACE_SIZE) / KT_TRACE_PART_SIZE;
	header = &trace->headers[part];

	kt_spin_lock(&trace->lock);

	if (header->clock > clock) {
		kt_spin_unlock(&trace->lock);
		memset(state, 0, sizeof(*state));
		state->cpu_id = -1;
		return;
	}

	memcpy(state, &header->state, sizeof(*state));
	end = clock % KT_TRACE_SIZE;
	beg = round_down(end, KT_TRACE_PART_SIZE);
	kt_trace_follow(trace, beg, end, state);

	event = trace->events[end];
	if (kt_event_has_pc(event.type) &&
	    state->stack.size + 1 < KT_MAX_STACK_FRAMES)
		kt_stack_push(&state->stack, event.data);

	kt_spin_unlock(&trace->lock);
}

void kt_trace_dump(kt_trace_t *trace, unsigned long beg, unsigned long end)
{
	kt_event_t event;
	unsigned long i;
	uptr_t pc;
	int cpu, pid;

	for (i = beg; i <= end; i++) {
		event = trace->events[i % KT_TRACE_SIZE];
		pc = kt_event_has_pc(event.type) ? kt_decompress(event.data) : 0;
		switch (event.type) {
		case kt_event_mop:
			pr_err(" i: %lu, access , pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_func_enter:
			pc = kt_decompress(event.data);
			pr_err(" i: %lu, enter  , pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_func_exit:
			pr_err(" i: %lu, exit\n", i);
			break;
		case kt_event_thr_start:
		case kt_event_thr_stop:
			cpu = event.data & 0xffff;
			pid = (s32)(u32)(event.data >> 16);
			pr_err(" i: %lu, %s, cpu: %d, thread: %d\n", i,
			       event.type == kt_event_thr_start ? "start  " :
								  "stop   ",
			       cpu, pid);
			break;
		case kt_event_lock:
			pr_err(" i: %lu, lock   , mutex: %llu\n", i,
			       (u64)event.data);
			i++;
			break;
		case kt_event_unlock:
			pr_err(" i: %lu, unlock , mutex: %llu\n", i,
			       (u64)event.data);
			break;
		case kt_event_rlock:
			pr_err(" i: %lu, rlock  , mutex: %llu\n", i,
			       (u64)event.data);
			i++;
			break;
		case kt_event_runlock:
			pr_err(" i: %lu, runlock, mutex: %llu\n", i,
			       (u64)event.data);
			break;
		case kt_event_downgrade:
			pr_err(" i: %lu, downgrade , mutex: %llu\n", i,
			       (u64)event.data);
			break;
		case kt_event_interrupt:
			pr_err(" i: %lu, interrupt\n", i);
			break;
#ifdef CONFIG_KTSAN_DEBUG
		case kt_event_preempt_enable:
			pr_err(" i: %lu, prm on , pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_preempt_disable:
			pr_err(" i: %lu, prm off, pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_irq_enable:
			pr_err(" i: %lu, irq on , pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_irq_disable:
			pr_err(" i: %lu, irq off, pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_acquire:
			pr_err(" i: %lu, acquire, pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_release:
			pr_err(" i: %lu, release, pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_nonmat_acquire:
			pr_err(" i: %lu, nm acq , pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_nonmat_release:
			pr_err(" i: %lu, nm rel , pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_membar_acquire:
			pr_err(" i: %lu, mb acq , pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_membar_release:
			pr_err(" i: %lu, mb rel , pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_event_enable:
			pr_err(" i: %lu, evt on , pc: %pS\n", i, (void *)pc);
			break;
		case kt_event_event_disable:
			pr_err(" i: %lu, evt off, pc: %pS\n", i, (void *)pc);
			break;
#endif
		default:
			break;
		}
	}
}
