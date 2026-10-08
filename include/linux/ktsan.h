#ifndef LINUX_KTSAN_H
#define LINUX_KTSAN_H

#include <linux/gfp.h>
#include <linux/bug.h>
#include <linux/init.h>
#include <linux/types.h>

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

#define KTSAN_WARN_ON WARN_ON

#endif /* CONFIG_KTSAN */

#endif /* LINUX_KTSAN_H */

