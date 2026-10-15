// SPDX-License-Identifier: GPL-2.0

//#include <asm/ktsan.h>
#include <asm/ktsan.h>
#include <asm/tlbflush.h>
#include <linux/cacheflush.h>
#include <linux/log2.h>
#include <linux/memblock.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/stddef.h>
#include <linux/vmalloc.h>

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

/*
 * Shadow of pages that have none of their own (see
 * ktsan_vmap_pages_range_noflush()). Shared by all of them, so its contents
 * are meaningless, but the vmalloc shadow region must be mapped wherever the
 * vmalloc area is.
 */
static char no_shadow_run[KT_SHADOW_RATIO * PAGE_SIZE] __aligned(PAGE_SIZE);

/*
 * Non-instrumented versions of is_vmalloc_addr() and is_module_address(),
 * adapted from KMSAN's kmsan_internal_is_vmalloc_addr() and
 * kmsan_internal_is_module_addr().
 */
static inline bool kt_is_vmalloc_addr(u64 addr)
{
	return addr >= VMALLOC_START && addr < VMALLOC_END;
}

static inline bool kt_is_module_addr(u64 addr)
{
	return addr >= MODULES_VADDR && addr < MODULES_END;
}

/*
 * Returns the address of the shadow of a vmalloc or module address in the
 * shadow region (see arch/x86/include/asm/pgtable_64_types.h), or 0 for any
 * other address. Adapted from KMSAN's vmalloc_meta().
 */
static u64 vmalloc_shadow(u64 addr)
{
	if (kt_is_vmalloc_addr(addr))
		return KTSAN_VMALLOC_SHADOW_START +
		       (addr - VMALLOC_START) * KT_SHADOW_RATIO;
	if (kt_is_module_addr(addr))
		return KTSAN_MODULES_SHADOW_START +
		       (addr - MODULES_VADDR) * KT_SHADOW_RATIO;
	return 0;
}

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
	u64 shadow, off;

	/*
	 * Mapped by ktsan_vmap_pages_range_noflush() and
	 * ktsan_ioremap_page_range(). TODO: ranges mapped behind their back
	 * (e.g. by vmap_pfn()) have no shadow mapped, and accesses to them
	 * fault.
	 */
	shadow = vmalloc_shadow(addr);
	if (shadow)
		return (void *)shadow;

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
			set_no_shadow_page(&shadow_p[i]);
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

/*
 * Clears the shadow of freshly allocated pages, so that they carry no history
 * from their previous user. Shadow handed over from memblock is not zeroed
 * either, so this must also run before KTSAN is enabled: hence no ENTER().
 */
void ktsan_alloc_page(struct page *page, unsigned int order)
{
	for (unsigned long i = 0; i < (1UL << order); i++) {
		if (!page_has_metadata(&page[i]))
			continue;
		memset(shadow_ptr_for(&page[i]), 0,
		       KT_SHADOW_RATIO * PAGE_SIZE);
	}
}

/*
 * Freeing pages is a write to all of them. Sync objects are dropped per
 * page: kt_memblock_addr() keys them by the head page, which for a
 * non-compound block of order > 0 may be any of its pages.
 */
void ktsan_free_page(struct page *page, unsigned int order)
{
	uptr_t addr = (uptr_t)page_address(page);

	ENTER(KT_ENTER_NORMAL);
	kt_access_range(thr, pc, addr, PAGE_SIZE << order, false);
	for (unsigned long i = 0; i < (1UL << order); i++)
		kt_memblock_free(thr, pc, addr + i * PAGE_SIZE, PAGE_SIZE,
				 false);
	LEAVE();
}

/*
 * The page contents are copied by uninstrumented code (copy_page()), so
 * carry the access history over with them. Adapted from KMSAN's
 * kmsan_copy_page_meta().
 */
void ktsan_copy_page_meta(struct page *dst, struct page *src)
{
	if (!dst || !page_has_metadata(dst))
		return;
	if (!src || !page_has_metadata(src)) {
		memset(shadow_ptr_for(dst), 0, KT_SHADOW_RATIO * PAGE_SIZE);
		return;
	}
	memcpy(shadow_ptr_for(dst), shadow_ptr_for(src),
	       KT_SHADOW_RATIO * PAGE_SIZE);
}

/* Data pages whose shadow ktsan_vmap_pages_range_noflush() maps at a time. */
#define KT_VMAP_BATCH 512

/*
 * Maps the shadow of @pages at the shadow address of [start, end). Each data
 * page has its own run of KT_SHADOW_RATIO shadow pages, and the runs of
 * neighbouring pages are not contiguous, so the shadow is always mapped with
 * small pages, whatever @page_shift is.
 *
 * Unlike KMSAN this runs before KTSAN is enabled too: vmalloc_init() comes
 * before ktsan_init_runtime(), and anything mapped there is accessed later.
 *
 * Adapted from KMSAN's kmsan_vmap_pages_range_noflush().
 */
int ktsan_vmap_pages_range_noflush(unsigned long start, unsigned long end,
				   pgprot_t prot, struct page **pages,
				   unsigned int page_shift, gfp_t gfp_mask)
{
	unsigned long shadow_start, shadow, nr, batch;
	struct page **s_pages, *run;
	int err = 0;

	shadow_start = vmalloc_shadow(start);
	if (!shadow_start)
		return 0;

	nr = (end - start) / PAGE_SIZE;
	batch = min(nr, (unsigned long)KT_VMAP_BATCH);
	s_pages = kmalloc_array(batch * KT_SHADOW_RATIO, sizeof(*s_pages),
				gfp_mask);
	if (!s_pages)
		return -ENOMEM;

	shadow = shadow_start;
	for (unsigned long i = 0; i < nr; i += batch) {
		unsigned long n = min(batch, nr - i);

		for (unsigned long k = 0; k < n; k++) {
			if (page_has_metadata(pages[i + k]))
				run = shadow_page_for(pages[i + k]);
			else
				run = virt_to_page(no_shadow_run);
			for (int j = 0; j < KT_SHADOW_RATIO; j++)
				s_pages[k * KT_SHADOW_RATIO + j] = &run[j];
		}
		err = __vmap_pages_range_noflush(
			shadow, shadow + n * KT_SHADOW_RATIO * PAGE_SIZE,
			PAGE_KERNEL, s_pages, PAGE_SHIFT);
		if (err) {
			__vunmap_range_noflush(shadow_start, shadow);
			break;
		}
		shadow += n * KT_SHADOW_RATIO * PAGE_SIZE;
	}
	kfree(s_pages);
	if (err)
		return err;

	/*
	 * ktsan_vunmap_range_noflush() leaves the TLB alone, so there may be
	 * stale entries for this range.
	 */
	flush_tlb_kernel_range(shadow_start, shadow);
	flush_cache_vmap(shadow_start, shadow);
	return 0;
}

/*
 * The shadow pages stay attached to the data pages, so stale TLB entries for
 * the range only point to the shadow of a page that is gone. They are flushed
 * when the range is mapped again.
 */
void ktsan_vunmap_range_noflush(unsigned long start, unsigned long end)
{
	unsigned long shadow_start = vmalloc_shadow(start);
	unsigned long shadow_end;

	if (!shadow_start)
		return;
	shadow_end = shadow_start + (end - start) * KT_SHADOW_RATIO;
	__vunmap_range_noflush(shadow_start, shadow_end);
	flush_cache_vmap(shadow_start, shadow_end);
}

#define KT_SHADOW_ORDER ilog2(KT_SHADOW_RATIO)

/*
 * Frees the shadow that ktsan_ioremap_page_range() allocated for
 * [start, start + nr pages).
 */
static void ktsan_free_io_shadow(unsigned long start, unsigned long nr)
{
	unsigned long shadow_start = vmalloc_shadow(start);
	unsigned long shadow_end = shadow_start +
				   nr * KT_SHADOW_RATIO * PAGE_SIZE;
	unsigned long shadow;
	struct page *page, *tmp;
	LIST_HEAD(freed);

	for (shadow = shadow_start; shadow < shadow_end;
	     shadow += KT_SHADOW_RATIO * PAGE_SIZE) {
		page = vmalloc_to_page((void *)shadow);
		if (page)
			list_add(&page->lru, &freed);
	}
	__vunmap_range_noflush(shadow_start, shadow_end);
	/* Nothing may write to the shadow pages once they are freed. */
	flush_tlb_kernel_range(shadow_start, shadow_end);
	list_for_each_entry_safe(page, tmp, &freed, lru) {
		list_del(&page->lru);
		__free_pages(page, KT_SHADOW_ORDER);
	}
}

/*
 * I/O memory has no struct page, so it gets fresh zeroed shadow, freed by
 * ktsan_iounmap_page_range(). Adapted from KMSAN's kmsan_ioremap_page_range().
 */
int ktsan_ioremap_page_range(unsigned long start, unsigned long end,
			     phys_addr_t phys_addr, pgprot_t prot,
			     unsigned int page_shift)
{
	struct page *run, *s_pages[KT_SHADOW_RATIO];
	unsigned long shadow, nr, i;
	int err = 0;

	if (!vmalloc_shadow(start))
		return 0;

	nr = (end - start) / PAGE_SIZE;
	for (i = 0; i < nr; i++) {
		run = alloc_pages(GFP_KERNEL | __GFP_ZERO, KT_SHADOW_ORDER);
		if (!run) {
			err = -ENOMEM;
			break;
		}
		for (int j = 0; j < KT_SHADOW_RATIO; j++)
			s_pages[j] = &run[j];
		shadow = vmalloc_shadow(start + i * PAGE_SIZE);
		err = __vmap_pages_range_noflush(
			shadow, shadow + KT_SHADOW_RATIO * PAGE_SIZE,
			PAGE_KERNEL, s_pages, PAGE_SHIFT);
		if (err) {
			__free_pages(run, KT_SHADOW_ORDER);
			break;
		}
	}
	if (err) {
		ktsan_free_io_shadow(start, i);
		return err;
	}
	flush_tlb_kernel_range(vmalloc_shadow(start), vmalloc_shadow(start) +
			       nr * KT_SHADOW_RATIO * PAGE_SIZE);
	flush_cache_vmap(vmalloc_shadow(start), vmalloc_shadow(start) +
			 nr * KT_SHADOW_RATIO * PAGE_SIZE);
	return 0;
}

void ktsan_iounmap_page_range(unsigned long start, unsigned long end)
{
	if (!vmalloc_shadow(start))
		return;
	ktsan_free_io_shadow(start, (end - start) / PAGE_SIZE);
}
