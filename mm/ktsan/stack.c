// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

#include <linux/kernel.h>
#include <linux/stacktrace.h>

void kt_stack_copy(kt_stack_t *dst, kt_stack_t *src)
{
	memcpy(&dst->pc[0], &src->pc[0], src->size * sizeof(src->pc[0]));
	dst->size = src->size;
}

void kt_stack_print(kt_stack_t *stack, uptr_t top_pc)
{
	uptr_t pc;
	int i;

	if (top_pc)
		pr_err(" %pS\n", (void *)top_pc);
	for (i = stack->size - 1; i >= 0; i--) {
		pc = kt_decompress(stack->pc[i]);
		pr_err(" %pS\n", (void *)pc);
	}
	pr_err("\n");
}

#ifdef CONFIG_KTSAN_DEBUG
void kt_stack_print_current(unsigned long strip_addr)
{
	kt_stack_t stack;

	kt_stack_save_current(&stack, strip_addr);
	kt_stack_print(&stack, 0);
}

void kt_stack_save_current(kt_stack_t *stack, unsigned long strip_addr)
{
	unsigned long entries[KT_MAX_STACK_FRAMES];
	unsigned int beg = 0, end, i;
	unsigned int nr_entries;

	nr_entries = stack_trace_save(entries, KT_MAX_STACK_FRAMES, 1);
	if (!nr_entries) {
		stack->size = 0;
		return;
	}
	end = nr_entries - 1;
	while (entries[beg] != strip_addr && beg < end)
		beg++;

	for (i = 0; i < end - beg; i++)
		stack->pc[i] = kt_compress(entries[end - 1 - i]);
	stack->size = end - beg;
}
#endif
