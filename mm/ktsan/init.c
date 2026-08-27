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

#ifdef CONFIG_KTSAN_DEBUG
static unsigned long kt_in, kt_freed, kt_shadow, kt_dropped __initdata;
static unsigned long kt_disc_freed, kt_disc_shadow __initdata;
#endif

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
 * collected later by ktsan_memblock_discard().
 *
 * Adapted from KMSAN's eager shadow memory allocation, but allocates
 * 4 shadow pages as metadata per 1 page instead of having 1 shadow page 
 * and 1 origin page.
 */
bool __init ktsan_memblock_free_pages(struct page *page, unsigned int order)
{
	struct metadata_blocks *hb;
	
	#ifdef CONFIG_KTSAN_DEBUG
	unsigned long num_pages = 1UL << order;
	kt_in += num_pages;
	#endif

	if (order < KT_SHADOW_SLOTS_LOG) {
		#ifdef CONFIG_KTSAN_DEBUG
		kt_dropped += 1UL << order;
		#endif
		return false;
	}

	hb = &held_back[order];
	if (hb->count < KT_SHADOW_RATIO) {
		hb->shadow[hb->count++] = page;
		return false;
	}
 
	ktsan_setup_meta(page, hb->shadow, order);
	hb->count = 0;

	#ifdef CONFIG_KTSAN_DEBUG
	kt_freed += num_pages;
	kt_shadow += num_pages * KT_SHADOW_RATIO;
	#endif

	return true;
}
 
#define MAX_BLOCKS 16
struct smallstack {
	struct page *items[MAX_BLOCKS];
	int index;
	int order;
};

static struct smallstack collect = {
	.index = 0,
	.order = MAX_PAGE_ORDER,
};

static void smallstack_push(struct smallstack *stack, struct page *pages)
{
	KTSAN_WARN_ON(stack->index == MAX_BLOCKS);
	stack->items[stack->index] = pages;
	stack->index++;
}
#undef MAX_BLOCKS

static struct page *smallstack_pop(struct smallstack *stack)
{
	struct page *ret;

	KTSAN_WARN_ON(stack->index == 0);
	stack->index--;
	ret = stack->items[stack->index];
	stack->items[stack->index] = NULL;
	return ret;
}

static void do_collection(void)
{
	struct page *page;
	struct metadata_blocks meta;

	while (collect.index >= KT_SHADOW_RATIO + 1) {
		page = smallstack_pop(&collect);
		for (meta.count = 0; meta.count < KT_SHADOW_RATIO; meta.count++)
			meta.shadow[meta.count] = smallstack_pop(&collect);
		
		ktsan_setup_meta(page, meta.shadow, collect.order);
		__free_pages_core(page, collect.order, MEMINIT_EARLY);
		
		#ifdef CONFIG_KTSAN_DEBUG
		kt_disc_freed += 1UL << collect.order;
		kt_disc_shadow += (1UL << collect.order) * KT_SHADOW_RATIO;
		#endif
	}
}

static void collect_split(void)
{
	struct smallstack tmp = {
		.order = collect.order - 1,
		.index = 0,
	};
	struct page *page;

	if (!collect.order)
		return;
	while (collect.index) {
		page = smallstack_pop(&collect);
		smallstack_push(&tmp, &page[0]);
		smallstack_push(&tmp, &page[1 << tmp.order]);
	}
	__memcpy(&collect, &tmp, sizeof(tmp));
}

/*
 * Memblock is about to go away. Split the page blocks left over in held_back[]
 * and return 1/5 of that memory to the system.
 */
static void ktsan_memblock_discard(void)
{
	/*
	 * For each order=N:
	 *  - push held_back[N].shadow[count] to @collect;
	 *  - while there are >= 5 elements in @collect, do garbage collection:
	 *    - pop 5 ranges from @collect;
	 *    - use four of them as shadow for the fifth one;
	 *    - repeat;
	 *  - split each remaining element from @collect into 2 ranges of
	 *    order=N-1,
	 *  - repeat.
	 */
	collect.order = MAX_PAGE_ORDER;
	struct metadata_blocks *hb;
	
	#ifdef CONFIG_KTSAN_DEBUG
	unsigned long held = 0;
	#endif
	
	int i, j;

	for (i = MAX_PAGE_ORDER; i >= 0; i--) {
		#ifdef CONFIG_KTSAN_DEBUG
		held += (unsigned long)held_back->count << i;
		#endif
		hb = &held_back[i];
		for (j = 0; j < hb->count; j++) {
			smallstack_push(&collect, hb->shadow[j]);
			hb->shadow[j] = NULL;
		}
		hb->count = 0;
		do_collection();
		collect_split();
	}

	#ifdef CONFIG_KTSAN_DEBUG
	pr_info("KTSAN: in=%lu freed=%lu shadow=%lu dropped=%lu\n", 
		kt_in, kt_disc_freed + kt_freed, kt_shadow + kt_disc_shadow, kt_dropped);
	pr_info("KTSAN: held=%lu balance=%ld ratio=%lu.%03lu\n", 
		held, (long)(kt_in - (kt_freed + kt_disc_freed + kt_shadow +
			kt_disc_shadow + kt_dropped + held)),
		kt_in / (kt_freed + kt_disc_freed),
		(kt_in % (kt_freed + kt_disc_freed)) * 1000 /
			(kt_freed + kt_disc_freed));
	#endif
}

void __init ktsan_init_runtime(void)
{
	ktsan_memblock_discard();
	pr_info("Starting KernelThreadSanitizer\n");
	pr_info("ATTENTION: KTSAN is a debugging tool, not for production use\n");
	ktsan_enabled = true;
}
