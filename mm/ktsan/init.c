// SPDX-License-Identifier: GPL-2.0

#include <asm/sections.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/ktsan.h>

#include "../internal.h"
#include "ktsan.h"

#define NUM_FUTURE_RANGES 128
struct start_end_pair {
	u64 start, end;
};

static struct start_end_pair start_end_pairs[NUM_FUTURE_RANGES] __initdata;
static int future_index __initdata;

bool ktsan_enabled __read_mostly;
int panic_on_ktsan __read_mostly;

/*
 * Record a range of memory for which the metadata pages will be created once
 * the page allocator becomes available.
 */
static void __init ktsan_record_future_shadow_range(void *start, void *end)
{
	u64 nstart = (u64)start, nend = (u64)end, cstart, cend;
	bool merged = false;

	KTSAN_WARN_ON(future_index == NUM_FUTURE_RANGES);
	KTSAN_WARN_ON((nstart >= nend) ||
		      /* Virtual address 0 is valid on s390. */
		      (!IS_ENABLED(CONFIG_S390) && !nstart) || !nend);
	nstart = ALIGN_DOWN(nstart, PAGE_SIZE);
	nend = ALIGN(nend, PAGE_SIZE);

	/*
	 * Scan the existing ranges to see if any of them overlaps with
	 * [start, end). In that case, merge the two ranges instead of
	 * creating a new one.
	 * The number of ranges is less than 20, so there is no need to organize
	 * them into a more intelligent data structure.
	 */
	for (int i = 0; i < future_index; i++) {
		cstart = start_end_pairs[i].start;
		cend = start_end_pairs[i].end;
		if ((cstart < nstart && cend < nstart) ||
		    (cstart > nend && cend > nend))
			/* ranges are disjoint - do not merge */
			continue;
		start_end_pairs[i].start = min(nstart, cstart);
		start_end_pairs[i].end = max(nend, cend);
		merged = true;
		break;
	}
	if (merged)
		return;
	start_end_pairs[future_index].start = nstart;
	start_end_pairs[future_index].end = nend;
	future_index++;
}

/*
 * Initialize the shadow for existing mappings during kernel initialization.
 * These include kernel text/data sections, NODE_DATA and future ranges
 * registered while creating other data (e.g. percpu).
 *
 * Allocations via memblock can be only done before slab is initialized.
 * 
 * Adapted from KMSAN's eager shadow memory allocation, but allocates
 * 4 shadow pages as metadata per 1 page instead of having 1 shadow page 
 * and 1 origin page.
 */
void __init ktsan_init_shadow(void)
{
	const size_t nd_size = sizeof(pg_data_t);
	phys_addr_t p_start, p_end;
	u64 loop;
	int nid;

	for_each_reserved_mem_range(loop, &p_start, &p_end)
		ktsan_record_future_shadow_range(phys_to_virt(p_start),
						 phys_to_virt(p_end));
	/* Allocate shadow for .data */
	ktsan_record_future_shadow_range(_sdata, _edata);

	for_each_online_node(nid)
		ktsan_record_future_shadow_range(
			NODE_DATA(nid), (char *)NODE_DATA(nid) + nd_size);

	for (int i = 0; i < future_index; i++)
		ktsan_init_alloc_meta_for_range(
			(void *)start_end_pairs[i].start,
			(void *)start_end_pairs[i].end);
}

struct metadata_blocks {
	struct page *shadow[KT_SHADOW_RATIO];
	unsigned int count;
};

static struct metadata_blocks held_back[NR_PAGE_ORDERS] __initdata;
 
/*
 * Eager metadata allocation. When the memblock allocator is freeing pages to
 * pagealloc, we use 4/5 of them as metadata for the remaining 1/5.
 * We store the pointers to the returned blocks of pages in held_back[] grouped
 * by their order: when ktsan_memblock_free_pages() with a certain order is called, 
 * the given block is reserved as a shadow block. 
 * When the certain number of blocks of the same order is stored, next incoming 
 * block receives its shadow range from the previously saved shadow blocks,
 * after which held_back[order] can be used again.
 *
 * At the very end there may be leftover blocks in held_back[]. They are
 * collected later by kmsan_memblock_discard().
 *
 * Adapted from KMSAN's eager shadow memory allocation, but allocates
 * 4 shadow pages as metadata per 1 page instead of having 1 shadow page 
 * and 1 origin page.
 */
bool __init ktsan_memblock_free_pages(struct page *page, unsigned int order)
{
	struct metadata_blocks *hb;
 
	if (order < KT_SHADOW_SLOTS_LOG)
		return false;
 
	hb = &held_back[order];
	if (hb->count < KT_SHADOW_RATIO) {
		hb->shadow[hb->count++] = page;
		return false;
	}
 
	ktsan_setup_meta(page, hb->shadow, order);
	hb->count = 0;
	return true;
}
 
void __init ktsan_init_runtime(void)
{
	pr_info("Starting KernelThreadSanitizer\n");
	pr_info("ATTENTION: KTSAN is a debugging tool, not for production use\n");
	ktsan_enabled = true;
}
