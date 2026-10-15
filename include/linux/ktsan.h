#ifndef LINUX_KTSAN_H
#define LINUX_KTSAN_H

#include <linux/gfp.h>
#include <linux/bug.h>
#include <linux/init.h>
#include <linux/types.h>

struct kmem_cache;
struct page;
struct task_struct;

#ifdef CONFIG_KTSAN

/*
 * ktsan_init_early() - Initializes structures:
 *
 *
 *
 */
void __init ktsan_init_early(void);

/*
 * ktsan_init_shadow() - Initializes KTSAN shadow at boot time.
 *
 * Allocate and initializes KTSAN metadata for early allocations.
 */
void __init ktsan_init_shadow(void);

/*
 * ktsan_memblock_free_pages() - handles handover of memblock pages.
 * @page:	struct page to free.
 * @order:	order of @page.
 *
 * Freed pages are either handed over to buddy allocator or held back
 * to be used as metadata.
 */
bool __init __must_check ktsan_memblock_free_pages(struct page *page,
				      unsigned int order);

/*
 * ktsan_init_runtime() - Initializes KTSAN's state and enables KTSAN.
 *
 * Also creates the KTSAN thread for task 0 (init_task), which is current
 * at this point. The thread is started later by ktsan_task_start().
 */
void __init ktsan_init_runtime(void);

/*
 * ktsan_task_create() - Creates the KTSAN thread for a new task.
 * @p:	task returned by copy_process(), not yet running.
 *
 * The new thread inherits the vector clock of the current (parent) thread.
 */
void ktsan_task_create(struct task_struct *p);

/*
 * ktsan_task_destroy() - Releases the KTSAN thread of a dead task.
 * @p:	task in TASK_DEAD state, called from the scheduler.
 */
void ktsan_task_destroy(struct task_struct *p);

/*
 * ktsan_task_start() - Marks the current task as running on this CPU.
 */
void ktsan_task_start(void);

/*
 * ktsan_task_stop() - Marks the current task as no longer running.
 */
void ktsan_task_stop(void);

/**
 * ktsan_alloc_page() - Notify KTSAN about an alloc_pages() call.
 * @page:  struct page pointer returned by alloc_pages().
 * @order: order of allocated struct page.
 *
 * Clears the shadow of the allocated pages.
 */
void ktsan_alloc_page(struct page *page, unsigned int order);

/**
 * ktsan_free_page() - Notify KTSAN about a free_pages() call.
 * @page:  struct page pointer passed to free_pages().
 * @order: order of deallocated struct page.
 *
 * Treats the free as a write to the pages and drops the sync objects that
 * live in them.
 */
void ktsan_free_page(struct page *page, unsigned int order);

/**
 * ktsan_slab_alloc() - Notify KTSAN about a slab allocation.
 * @s:      slab cache the object belongs to.
 * @object: object pointer, may be NULL.
 *
 * Marks the object as written by the current thread.
 */
void ktsan_slab_alloc(struct kmem_cache *s, void *object);

/**
 * ktsan_slab_free() - Notify KTSAN about a slab deallocation.
 * @s:                slab cache the object belongs to.
 * @object:           object pointer.
 * @still_accessible: the object may still be accessed (SLAB_TYPESAFE_BY_RCU).
 *
 * Treats the free as a write to the object, unless @still_accessible, and
 * drops the sync objects that live in it.
 */
void ktsan_slab_free(struct kmem_cache *s, void *object, bool still_accessible);

/**
 * ktsan_split_folio() - Notify KTSAN that a compound page is being split.
 * @page:      head page of the folio being split.
 * @old_order: order of the folio before the split.
 * @new_order: order of each of the resulting folios.
 *
 * Moves the sync objects of the folio to the new folios that contain them.
 * Splitting a non-compound page (split_page()) needs no hook.
 */
void ktsan_split_folio(struct page *page, unsigned int old_order,
		       unsigned int new_order);

/**
 * ktsan_copy_page_meta() - Copy KTSAN shadow between two pages.
 * @dst: destination page.
 * @src: source page.
 *
 * The page contents are copied by uninstrumented code, so carry the access
 * history over with them.
 */
void ktsan_copy_page_meta(struct page *dst, struct page *src);

/**
 * ktsan_vmap_pages_range_noflush() - Notify KTSAN about a vmap.
 * @start:     start of vmapped range.
 * @end:       end of vmapped range.
 * @prot:      page protection flags used for vmap.
 * @pages:     array of pages.
 * @page_shift: page_shift passed to vmap_range_noflush().
 * @gfp_mask:  gfp_mask to use internally.
 *
 * Maps the shadow of @pages into the shadow region of the vmalloc area.
 *
 * Return: 0 on success, or a negative error code.
 */
int __must_check ktsan_vmap_pages_range_noflush(unsigned long start,
						unsigned long end,
						pgprot_t prot,
						struct page **pages,
						unsigned int page_shift,
						gfp_t gfp_mask);

/**
 * ktsan_vunmap_range_noflush() - Notify KTSAN about a vunmap.
 * @start: start of vunmapped range.
 * @end:   end of vunmapped range.
 */
void ktsan_vunmap_range_noflush(unsigned long start, unsigned long end);

/**
 * ktsan_ioremap_page_range() - Notify KTSAN about a ioremap_page_range() call.
 * @addr:      range start.
 * @end:       range end.
 * @phys_addr: physical range start.
 * @prot:      page protection flags used for ioremap_page_range().
 * @page_shift: page_shift argument passed to vmap_range_noflush().
 *
 * I/O memory has no struct page, so this allocates fresh shadow for it.
 *
 * Return: 0 on success, or a negative error code.
 */
int __must_check ktsan_ioremap_page_range(unsigned long addr, unsigned long end,
					  phys_addr_t phys_addr, pgprot_t prot,
					  unsigned int page_shift);

/**
 * ktsan_iounmap_page_range() - Notify KTSAN about a iounmap_page_range() call.
 * @start: range start.
 * @end:   range end.
 */
void ktsan_iounmap_page_range(unsigned long start, unsigned long end);

/*
 * ktsan_interrupt_enter() - Marks the start of a hard interrupt.
 *
 * The interrupt runs on the interrupted thread, with its in-flight state
 * (locks, pending acquire/release, ...) set aside until the matching
 * ktsan_interrupt_exit() of the outermost interrupt.
 */
void ktsan_interrupt_enter(void);

/*
 * ktsan_interrupt_exit() - Marks the end of a hard interrupt.
 */
void ktsan_interrupt_exit(void);

extern bool ktsan_enabled;
extern int panic_on_ktsan;

/*
 * KTSAN performs a lot of consistency checks that are currently enabled by
 * default. BUG_ON is normally discouraged in the kernel, unless used for
 * debugging, but KTSAN itself is a debugging tool, so it makes little sense to
 * recover if something goes wrong.
 */
#define KTSAN_WARN_ON(cond)                                           \
	({                                                            \
		const bool __cond = WARN_ON(cond);                    \
		if (unlikely(__cond)) {                               \
			WRITE_ONCE(ktsan_enabled, false);             \
			if (panic_on_ktsan) {                         \
				/* Can't call panic() here because */ \
				/* of uaccess checks. */              \
				BUG();                                \
			}                                             \
		}                                                     \
		__cond;                                               \
	})

#else /* CONFIG_KTSAN */

static inline void ktsan_init_early(void)
{
}

static inline void ktsan_init_shadow(void) 
{
}
 
static inline bool ktsan_memblock_free_pages(struct page *page,
					     unsigned int order)
{
	return true;
}

static inline void ktsan_init_runtime(void)
{
}

static inline void ktsan_task_create(struct task_struct *p)
{
}

static inline void ktsan_task_destroy(struct task_struct *p)
{
}

static inline void ktsan_task_start(void)
{
}

static inline void ktsan_task_stop(void)
{
}

static inline void ktsan_alloc_page(struct page *page, unsigned int order)
{
}

static inline void ktsan_free_page(struct page *page, unsigned int order)
{
}

static inline void ktsan_slab_alloc(struct kmem_cache *s, void *object)
{
}

static inline void ktsan_slab_free(struct kmem_cache *s, void *object,
				   bool still_accessible)
{
}

static inline void ktsan_split_folio(struct page *page, unsigned int old_order,
				     unsigned int new_order)
{
}

static inline void ktsan_copy_page_meta(struct page *dst, struct page *src)
{
}

static inline int __must_check ktsan_vmap_pages_range_noflush(
	unsigned long start, unsigned long end, pgprot_t prot,
	struct page **pages, unsigned int page_shift, gfp_t gfp_mask)
{
	return 0;
}

static inline void ktsan_vunmap_range_noflush(unsigned long start,
					      unsigned long end)
{
}

static inline int __must_check ktsan_ioremap_page_range(unsigned long start,
							unsigned long end,
							phys_addr_t phys_addr,
							pgprot_t prot,
							unsigned int page_shift)
{
	return 0;
}

static inline void ktsan_iounmap_page_range(unsigned long start,
					    unsigned long end)
{
}

static inline void ktsan_interrupt_enter(void)
{
}

static inline void ktsan_interrupt_exit(void)
{
}

#define KTSAN_WARN_ON WARN_ON

#endif /* CONFIG_KTSAN */

#endif /* LINUX_KTSAN_H */

