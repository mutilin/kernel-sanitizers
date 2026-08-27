// SPDX-License-Identifier: GPL-2.0

//#include <asm/ktsan.h>
#include <asm/ktsan.h>
#include <linux/memblock.h>
#include <linux/mm_types.h>
#include <linux/smp.h>
#include <linux/stddef.h>

#include "../internal.h"
#include "ktsan.h"

#define shadow_page_for(page) ((page)->ktsan_shadow)

static void *shadow_ptr_for(struct page *page)
{
	return page_address(shadow_page_for(page));
}

static bool page_has_metadata(struct page *page)
{
	return shadow_page_for(page);
}

static void set_no_shadow_page(struct page *page)
{
	shadow_page_for(page) = NULL;
}

/*
 * Dummy load and store pages to be used when the real metadata is unavailable.
 * There are separate pages for loads and stores, so that every load returns a
 * zero, and every store doesn't affect other loads.
 */
static char dummy_load_page[PAGE_SIZE] __aligned(PAGE_SIZE);
static char dummy_store_page[PAGE_SIZE] __aligned(PAGE_SIZE);

/*static unsigned long vmalloc_meta(void *addr, bool is_origin)
{
	unsigned long addr64 = (unsigned long)addr, off;

	KMSAN_WARN_ON(is_origin && !IS_ALIGNED(addr64, KMSAN_ORIGIN_SIZE));
	if (kmsan_internal_is_vmalloc_addr(addr)) {
		off = addr64 - VMALLOC_START;
		return off + (is_origin ? KMSAN_VMALLOC_ORIGIN_START :
					  KMSAN_VMALLOC_SHADOW_START);
	}
	if (kmsan_internal_is_module_addr(addr)) {
		off = addr64 - MODULES_VADDR;
		return off + (is_origin ? KMSAN_MODULES_ORIGIN_START :
					  KMSAN_MODULES_SHADOW_START);
	}
	return 0;
}*/

static struct page *virt_to_page_or_null(void *vaddr)
{
	if (ktsan_virt_addr_valid(vaddr))
		return virt_to_page(vaddr);
	else
		return NULL;
}

void *ktsan_get_shadow_ptr(void *address, u64 size, bool store)
{
	void *shadow;
 
	/*
	 * Even redirected to the dummy region, a larger access would run out
	 * of bounds.
	 */
	KTSAN_WARN_ON(size > PAGE_SIZE);
 
	if (!ktsan_enabled)
		goto return_dummy;
 
	shadow = ktsan_get_shadow(address);
	if (!shadow)
		goto return_dummy;
 
	return shadow;
 
return_dummy:
	/* Ignore this store / return zeroes for this load. */
	return store ? dummy_store_page : dummy_load_page;
}

/*
 * Obtain the shadow or origin pointer for the given address, or NULL if there's
 * none. The caller must check the return value for being non-NULL if needed.
 * The return value of this function should not depend on whether we're in the
 * runtime or not.
 */
void *ktsan_get_shadow(void *address)
{
	u64 addr = ALIGN_DOWN((u64)address, KT_GRAIN);
	struct page *page;
	u64 off;
 
	/*
	 * TODO: vmalloc and module ranges have no shadow yet.  KMSAN maps a
	 * parallel VA region for them in vmalloc_meta(); until that exists,
	 * accesses there fall through to the dummy shadow.
	 */
 
	page = virt_to_page_or_null((void *)addr);
	if (!page || !page_has_metadata(page))
		return NULL;
 
	off = offset_in_page(addr) * KT_SHADOW_RATIO;
 
	return shadow_ptr_for(page) + off;
}

/* Allocates metadata for pages allocated at boot time. 
 * 
 * Adapted from KMSAN's eager shadow memory allocation, but allocates
 * 4 shadow pages as metadata per 1 page instead of having 1 shadow page 
 * and 1 origin page. 
 */
void __init ktsan_init_alloc_meta_for_range(void *start, void *end) 
{
	struct page *shadow_p;
	void *shadow;
	struct page *page;
	u64 size;

	start = (void *)PAGE_ALIGN_DOWN((u64)start);
	size = PAGE_ALIGN((u64)end - (u64)start);
	shadow = memblock_alloc_or_panic(size * KT_SHADOW_RATIO, PAGE_SIZE);

	for (u64 addr = 0; addr < size; addr += PAGE_SIZE) {
		page = virt_to_page_or_null((char *)start + addr);
		if (!page) 
			continue; // TODO: why can be skipped 
		shadow_p = virt_to_page((char *)shadow + addr * KT_SHADOW_RATIO);
		for (int i = 0; i < KT_SHADOW_RATIO; i++)
			set_no_shadow_page(shadow_p);
		shadow_page_for(page) = shadow_p;
		
	}
}

/*
 * Links the metadata with the corresponding block.
 *
 * Adapted from KMSAN's eager shadow memory allocation, but allocates
 * 4 shadow pages as metadata per 1 page instead of having 1 shadow page 
 * and 1 origin page. 
 */
void ktsan_setup_meta(struct page *page, struct page **shadow, unsigned int order)
{
	const unsigned long nr_pages = 1UL << order;
	/* Data pages covered by one shadow block. */
	const unsigned long per_block = nr_pages >> KT_SHADOW_SLOTS_LOG;
 
	if (KTSAN_WARN_ON(!per_block))
		return;
	for (unsigned long i = 0; i < nr_pages; i++) {
		struct page *run = &shadow[i / per_block]
					 [(i % per_block) << KT_SHADOW_SLOTS_LOG];
 
		for (int j = 0; j < KT_SHADOW_RATIO; j++)
			set_no_shadow_page(&run[j]);
 
		shadow_page_for(&page[i]) = run;
	}
}

