// SPDX-License-Identifier: GPL-2.0
#ifndef __X86_MM_KTSAN_KTSAN_H
#define __X86_MM_KTSAN_KTSAN_H

#include <linux/init.h>
#include <linux/ktsan.h>
#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <linux/types.h>

#define KT_GRAIN 8
#define KT_SHADOW_SLOTS_LOG 2
#define KT_SHADOW_SLOTS (1 << KT_SHADOW_SLOTS_LOG) 
#define KT_SHADOW_RATIO (KT_SHADOW_SLOTS * sizeof(struct kt_shadow_s) / KT_GRAIN) 

#define KT_THREAD_ID_BITS 12
#define KT_CLOCK_BITS 42

typedef unsigned long kt_time_t;

/* Clocks. */

/*struct kt_clk_s {
	kt_time_t time[KT_MAX_THREAD_COUNT];
};*/

/* Shadow. */

struct kt_shadow_s {
	unsigned long tid : KT_THREAD_ID_BITS;
	unsigned long clock : KT_CLOCK_BITS;
	unsigned long offset : 3;
	unsigned long size : 2;
	unsigned long read : 1;
	unsigned long atomic : 1;
};

/*
 * Shadow address for @address, or NULL when the page carries no metadata.
 * Independent of whether we are inside the runtime.
 */
void *ktsan_get_shadow(void *address);
 
/*
 * Shadow address for an access of @size bytes, falling back to a dummy region
 * when no real metadata exists.  Never returns NULL, so instrumentation
 * callbacks need no null check.
 */
void *ktsan_get_shadow_ptr(void *address, u64 size, bool store);
 
/* Boot-time metadata setup. */
void __init ktsan_init_alloc_meta_for_range(void *start, void *end);
void __init ktsan_setup_meta(struct page *page, struct page **shadow,
			     unsigned int order);

#endif /* __X86_MM_KTSAN_KTSAN_H */
