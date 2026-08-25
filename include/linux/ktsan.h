#ifndef LINUX_KTSAN_H
#define LINUX_KTSAN_H

#include <linux/gfp.h>
#include <linux/bug.h>
#include <linux/init.h>
#include <linux/types.h>

#ifdef CONFIG_KTSAN

/**
 * ktsan_init_shadow() - Initializes KTSAN shadow at boot time.
 *
 * Allocate and initializes KTSAN metadata for early allocations.
 */
void __init ktsan_init_shadow(void);
void __init ktsan_init_runtime(void);
bool __init __must_check ktsan_memblock_free_pages(struct page *page,
				      unsigned int order);

extern bool ktsan_enabled;
extern int panic_on_ktsan;

// TODO - fix the text
/*
 * KTSAN performs a lot of consistency checks that are currently enabled by
 * default. BUG_ON is normally discouraged in the kernel, unless used for
 * debugging, but KMSAN itself is a debugging tool, so it makes little sense to
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

static inline void ktsan_init_shadow(void) 
{
}

static inline void ktsan_init_runtime(void)
{
}
 
static inline bool ktsan_memblock_free_pages(struct page *page,
					     unsigned int order)
{
	return true;
}
 
#define KTSAN_WARN_ON WARN_ON

#endif /* CONFIG_KTSAN */

#endif /* LINUX_KTSAN_H */

