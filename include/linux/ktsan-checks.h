/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KTSAN hooks for synchronization primitives.
 *
 * Kept apart from <linux/ktsan.h> so that low-level headers (spinlock.h,
 * rwlock.h) can include it: <linux/ktsan.h> pulls in <linux/gfp.h>, which
 * includes spinlock.h back. Only <linux/types.h> may be included here.
 */
#ifndef _LINUX_KTSAN_CHECKS_H
#define _LINUX_KTSAN_CHECKS_H

#include <linux/types.h>

#ifdef CONFIG_KTSAN

/*
 * ktsan_sync_acquire() - Acquires the happens-before released into @addr.
 * @addr: any address used as the key of the edge.
 *
 * For synchronization KTSAN can't see on its own. Everything the releasing
 * thread did before ktsan_sync_release(@addr) happens before everything the
 * current thread does after this call.
 */
void ktsan_sync_acquire(void *addr);

/*
 * ktsan_sync_release() - Releases the current thread's clock into @addr.
 * @addr: any address used as the key of the edge.
 */
void ktsan_sync_release(void *addr);

/*
 * Lock hooks. @addr is the lock, @write tells an exclusive lock from a shared
 * (read) one, @trylock a trylock from a blocking lock.
 *
 * ktsan_mtx_post_lock() goes right after the lock is taken, with @success
 * false when it was not (failed trylock, interrupted sleeping lock).
 * ktsan_mtx_pre_unlock() goes right before the lock is dropped, while it is
 * still held, so that the release lands before the next owner's acquire.
 * ktsan_mtx_pre_lock() and ktsan_mtx_post_unlock() are placeholders for
 * deadlock detection and do nothing yet.
 */
void ktsan_mtx_pre_lock(void *addr, bool write, bool trylock);
void ktsan_mtx_post_lock(void *addr, bool write, bool trylock, bool success);
void ktsan_mtx_pre_unlock(void *addr, bool write);
void ktsan_mtx_post_unlock(void *addr, bool write);

/*
 * ktsan_mtx_downgrade() - An exclusive hold of @addr becomes a shared one
 * (downgrade_write()).
 */
void ktsan_mtx_downgrade(void *addr);

#else /* CONFIG_KTSAN */

static inline void ktsan_sync_acquire(void *addr)
{
}

static inline void ktsan_sync_release(void *addr)
{
}

static inline void ktsan_mtx_pre_lock(void *addr, bool write, bool trylock)
{
}

static inline void ktsan_mtx_post_lock(void *addr, bool write, bool trylock,
				       bool success)
{
}

static inline void ktsan_mtx_pre_unlock(void *addr, bool write)
{
}

static inline void ktsan_mtx_post_unlock(void *addr, bool write)
{
}

static inline void ktsan_mtx_downgrade(void *addr)
{
}

#endif /* CONFIG_KTSAN */

#endif /* _LINUX_KTSAN_CHECKS_H */
