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

#define KT_ACCESS_SIZE_1 0
#define KT_ACCESS_SIZE_2 1
#define KT_ACCESS_SIZE_4 2
#define KT_ACCESS_SIZE_8 3
 
#define KT_THREAD_ID_BITS 12
#define KT_CLOCK_BITS 42	


#define KT_MAX_THREAD_COUNT 128
#define KT_MAX_TASK_COUNT KT_MAX_THREAD_COUNT

#define KT_SYNC_TAB_SIZE 196613 /* prime: hash partition count */
#define KT_MEMBLOCK_TAB_SIZE 196613
 
#define KT_MAX_SYNC_COUNT (100 * 1000)
#define KT_MAX_MEMBLOCK_COUNT (50 * 1000)
#define KT_MAX_PERCPU_SYNC_COUNT (30 * 1000)
 
#define KT_MAX_STACK_FRAMES 96
#define KT_MAX_LOCKED_MTX 32
 
#define KT_STACK_DEPOT_PARTS 49157 /* prime */
/* Handles are (offset / 4) in a u32, so this can never exceed 16 GB. */
#define KT_STACK_DEPOT_MEMORY_LIMIT (16 << 20)
 
#define KT_TRACE_PARTS 8
#define KT_TRACE_PART_SIZE (1 << 10)
#define KT_TRACE_SIZE (KT_TRACE_PARTS * KT_TRACE_PART_SIZE)


#if CONFIG_KTSAN_DEBUG
#define KT_BUG_ON(x) BUG_ON(x)
#else
#define KT_BUG_ON(x)                                                           \
	{                                                                      \
	}
#endif

typedef unsigned long uptr_t;
typedef unsigned long kt_time_t;

typedef struct kt_thr_s kt_thr_t;
typedef struct kt_clk_s kt_clk_t;
typedef struct kt_tab_s kt_tab_t;
typedef struct kt_tab_obj_s kt_tab_obj_t;
typedef struct kt_tab_part_s kt_tab_part_t;
typedef struct kt_tab_sync_s kt_tab_sync_t;
typedef struct kt_tab_memblock_s kt_tab_memblock_t;
typedef struct kt_tab_lock_s kt_tab_lock_t;
typedef struct kt_tab_test_s kt_tab_test_t;
typedef struct kt_ctx_s kt_ctx_t;
typedef enum kt_stat_e kt_stat_t;
typedef struct kt_stats_s kt_stats_t;
typedef struct kt_cpu_s kt_cpu_t;
typedef struct kt_task_s kt_task_t;
typedef struct kt_race_info_s kt_race_info_t;
typedef struct kt_cache_s kt_cache_t;
typedef struct kt_stack_s kt_stack_t;
typedef u32 kt_stack_handle_t;
typedef struct kt_stack_depot_s kt_stack_depot_t;
typedef struct kt_stack_depot_obj_s kt_stack_depot_obj_t;
typedef enum kt_event_type_e kt_event_type_t;
typedef struct kt_event_s kt_event_t;
typedef struct kt_locked_mutex_s kt_locked_mutex_t;
typedef struct kt_mutexset_s kt_mutexset_t;
typedef struct kt_trace_state_s kt_trace_state_t;
typedef struct kt_trace_part_header_s kt_trace_part_header_t;
typedef struct kt_trace_s kt_trace_t;
typedef struct kt_id_manager_s kt_id_manager_t;
typedef struct kt_thr_pool_s kt_thr_pool_t;
typedef struct kt_shadow_s kt_shadow_t;
typedef struct kt_percpu_sync_s kt_percpu_sync_t;
typedef struct kt_spinlock_s kt_spinlock_t;
typedef struct kt_interrupted_s kt_interrupted_t;

/* Ktsan runtime internal, non-instrumented spinlock. */

struct kt_spinlock_s {
	u8 state;
};

/* Internal allocator. */

struct kt_cache_s {
	unsigned long base;
	unsigned long mem_size;
	void *head;
	kt_spinlock_t lock;
};

/* Stack. */

struct kt_stack_s {
	s32 size;
	u32 pc[KT_MAX_STACK_FRAMES];
};

/* Stack depot. */

struct kt_stack_depot_obj_s {
	kt_stack_depot_obj_t *link;
	u32 hash;
	s32 stack_size;
	u32 stack_pc[0];
};

struct kt_stack_depot_s {
	kt_cache_t stack_cache;
	u64 stack_offset;
	unsigned long nstacks;
	kt_stack_depot_obj_t *parts[KT_STACK_DEPOT_PARTS];
	kt_spinlock_t lock;
};

/* Trace. */

enum kt_event_type_e {
	kt_event_nop,
	kt_event_mop, /* memory operation */
	kt_event_func_enter,
	kt_event_func_exit,
	kt_event_thr_start,
	kt_event_thr_stop,
	kt_event_lock,
	kt_event_rlock,
	kt_event_unlock,
	kt_event_runlock,
	kt_event_interrupt,
	kt_event_downgrade,
#if CONFIG_KTSAN_DEBUG
	kt_event_acquire,
	kt_event_release,
	kt_event_nonmat_acquire,
	kt_event_nonmat_release,
	kt_event_membar_acquire,
	kt_event_membar_release,
	kt_event_preempt_enable,
	kt_event_preempt_disable,
	kt_event_irq_enable,
	kt_event_irq_disable,
	kt_event_event_disable,
	kt_event_event_enable,
#endif /* CONFIG_KTSAN_DEBUG */
};

struct kt_event_s {
	u64 type : 16;
	u64 data : 48;
	/* The data field is
	   cpu id for thread start and stop events,
	   sync uid for lock and unlock events,
	   and pc for other kinds of event. */
};

struct kt_locked_mutex_s {
	u64 uid;
	kt_stack_handle_t stack;
	bool write;
};

struct kt_mutexset_s {
	kt_locked_mutex_t mtx[KT_MAX_LOCKED_MTX];
	int size;
};

struct kt_trace_state_s {
	kt_stack_t stack;
	kt_mutexset_t mutexset;
	int pid;
	int cpu_id;
};

struct kt_trace_part_header_s {
	kt_trace_state_t state;
	kt_time_t clock;
};

struct kt_trace_s {
	kt_trace_part_header_t headers[KT_TRACE_PARTS];
	kt_event_t events[KT_TRACE_SIZE];
	kt_spinlock_t lock;
};

/* Clocks. */

struct kt_clk_s {
	kt_time_t time[KT_MAX_THREAD_COUNT];
};


/* Shadow. */

struct kt_shadow_s {
	unsigned long tid : KT_THREAD_ID_BITS;
	unsigned long clock : KT_CLOCK_BITS;
	unsigned long offset : 3;
	unsigned long size : 2;
	unsigned long read : 1;
	unsigned long atomic : 1;
};

/* Reports. */

struct kt_race_info_s {
	unsigned long addr;
	kt_shadow_t old;
	kt_shadow_t new;
};

/* Hash table. */

struct kt_tab_obj_s {
	kt_spinlock_t lock;
	kt_tab_obj_t *link;
	uptr_t key;
};

struct kt_tab_part_s {
	kt_spinlock_t lock;
	kt_tab_obj_t *head;
};

struct kt_tab_s {
	unsigned size;
	unsigned objsize;
	kt_tab_part_t *parts;
	kt_cache_t obj_cache;
	kt_cache_t parts_cache;
};

struct kt_tab_sync_s {
	kt_tab_obj_t tab;
	u64 uid;
	kt_clk_t clk;
	int lock_tid; /* id of thread that locked mutex */
	struct list_head list;
	uptr_t pc;
	kt_time_t last_lock_time;
	kt_time_t last_unlock_time;
};

struct kt_tab_lock_s {
	kt_tab_obj_t tab;
	kt_spinlock_t lock;
	struct list_head list;
};

struct kt_tab_memblock_s {
	kt_tab_obj_t tab;
	struct list_head sync_list;
	struct list_head lock_list;
};

struct kt_tab_test_s {
	kt_tab_obj_t tab;
	unsigned long data[4];
};

/* Threads. */

struct kt_thr_s {
	int id;
	int pid;
	unsigned long inside; /* already inside of ktsan runtime */
	kt_cpu_t *cpu;
	kt_clk_t clk;
	kt_stack_t stack;
	kt_mutexset_t mutexset;
	kt_clk_t acquire_clk;
	int acquire_active;
	kt_clk_t release_clk;
	int release_active;
	kt_trace_t trace;
	int read_disable_depth;
	int event_disable_depth;
	int report_disable_depth;
	int preempt_disable_depth;
	bool irqs_disabled;
	unsigned long irq_flags_before_disable;
	struct list_head quarantine_list; /* list entry */
	struct list_head percpu_list; /* list head */
	/* List of currently "acquired" for reading seqcounts. */
	uptr_t seqcount[6];
	/* Where the seqcounts were acquired (for debugging). */
	uptr_t seqcount_pc[6];
	/* Ignore of all seqcount-related events. */
	int seqcount_ignore;
	int interrupt_depth;
#if CONFIG_KTSAN_DEBUG
	kt_time_t last_event_disable_time;
	kt_time_t last_event_enable_time;
#endif
};

/* Holds state of an interrupted thread while it executes interrupts.
 * Essentially a subset of kt_thr_t state.
 */
struct kt_interrupted_s {
	kt_thr_t *thr;
	kt_stack_t stack;
	kt_mutexset_t mutexset;
	kt_clk_t acquire_clk;
	int acquire_active;
	kt_clk_t release_clk;
	int release_active;
	int read_disable_depth;
	int report_disable_depth;
	int preempt_disable_depth;
	struct list_head percpu_list;
	uptr_t seqcount[6];
	uptr_t seqcount_pc[6];
	int seqcount_ignore;
};

struct kt_thr_pool_s {
	kt_cache_t cache;
	kt_thr_t *thrs[KT_MAX_THREAD_COUNT];
	int new_id;
	int new_pid;
	struct list_head quarantine;
	int quarantine_size;
	kt_spinlock_t lock;
};

/* Per-cpu synchronization. */

struct kt_percpu_sync_s {
	uptr_t addr;
	struct list_head list;
};

/* Statistics. */

enum kt_stat_e {
	kt_stat_reports,
	kt_stat_access_read,
	kt_stat_access_write,
	kt_stat_access_size1,
	kt_stat_access_size2,
	kt_stat_access_size4,
	kt_stat_access_size8,
	kt_stat_sync_objects,
	kt_stat_sync_alloc,
	kt_stat_sync_free,
	kt_stat_memblock_objects,
	kt_stat_memblock_alloc,
	kt_stat_memblock_free,
	kt_stat_threads,
	kt_stat_thread_create,
	kt_stat_thread_destroy,
	kt_stat_acquire,
	kt_stat_release,
	kt_stat_func_entry,
	kt_stat_func_exit,
	kt_stat_trace_event,
	kt_stat_count,
};

struct kt_stats_s {
	unsigned long stat[kt_stat_count];
};

/* KTSAN per-cpu state. */

struct kt_cpu_s {
	/* Thread that currently runs on the CPU or NULL. */
	kt_thr_t *thr;
	kt_stats_t stat;
	u64 sync_uid_pos;
	u64 sync_uid_end;
	kt_interrupted_t interrupted;
};

/* KTSAN per-task state. */

struct kt_task_s {
	/* Thread that is associated with this task. Never NULL. */
	kt_thr_t *thr;
	/* Shows whether this task is being executed. */
	bool running;
};

/* Global. */

struct kt_ctx_s {
	int enabled;
	kt_cpu_t __percpu *cpus;
	kt_cache_t task_cache;
	kt_tab_t sync_tab; /* sync addr -> sync object */
	kt_tab_t memblock_tab; /* memory block -> sync objects */
	kt_tab_t test_tab;
	kt_cache_t percpu_sync_cache;
	kt_thr_pool_t thr_pool;
	kt_stack_depot_t stack_depot;
	u64 sync_uid_gen;
};

extern kt_ctx_t kt_ctx;

/* Statistics. Enabled only when KT_ENABLE_STATS = 1. */

void kt_stat_init(void);

static inline void kt_stat_add(kt_stat_t what, unsigned long x)
{
	if(!IS_ENABLED(CONFIG_KTSAN_STATS))
		return;
	this_cpu_ptr(kt_ctx.cpus)->stat.stat[what] += x;
}

static inline void kt_stat_inc(kt_stat_t what)
{
	kt_stat_add(what, 1);
}

static inline void kt_stat_dec(kt_stat_t what)
{
	kt_stat_add(what, -1);
}

/* Stack. */

/* All kernel addresses have 0xffff in high 2 bytes (on x86_64). */
#define KT_ADDR_MASK 0xffff000000000000ull
#define KT_PC_MASK 0xffffffff00000000ull

static inline u64 kt_compress(u64 addr)
{
	KT_BUG_ON((addr | KT_ADDR_MASK) != addr);
	return addr & ~KT_ADDR_MASK;
}

static inline u64 kt_decompress(u64 addr)
{
	KT_BUG_ON((addr & KT_ADDR_MASK) != 0);
	if (addr & KT_PC_MASK)
		return addr | KT_ADDR_MASK;
	/* This must be a PC with cut off high part. */
	return addr | KT_PC_MASK;
}

static __always_inline void kt_stack_init(kt_stack_t *stack)
{
	stack->size = 0;
}

static __always_inline void kt_stack_push(kt_stack_t *stack, u32 pc)
{
	BUG_ON(stack->size + 1 >= KT_MAX_STACK_FRAMES);
	stack->pc[stack->size++] = pc;
}

static __always_inline u32 kt_stack_pop(kt_stack_t *stack)
{
	BUG_ON(stack->size <= 0);
	return stack->pc[--stack->size];
}

void kt_stack_copy(kt_stack_t *dst, kt_stack_t *src);
void kt_stack_print(kt_stack_t *stack, uptr_t top_pc);

#if CONFIG_KTSAN_DEBUG
void kt_stack_print_current(unsigned long strip_addr);
void kt_stack_save_current(kt_stack_t *stack, unsigned long strip_addr);
#endif

/* Stack depot. */

void kt_stack_depot_init(kt_stack_depot_t *depot);
kt_stack_handle_t kt_stack_depot_save(kt_stack_depot_t *depot,
				      kt_stack_t *stack);
kt_stack_t *kt_stack_depot_get(kt_stack_depot_t *depot,
			       kt_stack_handle_t handle);
void kt_stack_depot_stats(kt_stack_depot_t *depot, unsigned long *nstacks,
			  unsigned long *memory);

/* Clocks. */

void kt_clk_init(kt_clk_t *clk);
void kt_clk_acquire(kt_clk_t *dst, kt_clk_t *src);
void kt_clk_set(kt_clk_t *dst, kt_clk_t *src);

static __always_inline kt_time_t kt_clk_get(kt_clk_t *clk, int tid)
{
	KT_BUG_ON(tid >= KT_MAX_THREAD_COUNT);
	return clk->time[tid];
}

static __always_inline void kt_clk_tick(kt_clk_t *clk, int tid)
{
	KT_BUG_ON(tid >= KT_MAX_THREAD_COUNT);
	clk->time[tid]++;
}

/* Trace. */

void kt_trace_init(kt_trace_t *trace);
void kt_trace_switch(kt_thr_t *thr);
void kt_trace_restore_state(kt_thr_t *thr, kt_time_t clock,
			    kt_trace_state_t *state);
void kt_trace_dump(kt_trace_t *trace, unsigned long beg, unsigned long end);
u64 kt_trace_last_data(kt_thr_t *thr);

/* Adds the event to the thread trace.
 * Only 48 low bits of data are saved, use kt_compress if you need to save
 * addresses or pcs.
 */
static inline void kt_trace_add_event(kt_thr_t *thr, kt_event_type_t type,
				      u64 data)
{
	kt_trace_t *trace;
	kt_time_t clock;
	kt_event_t event;
	unsigned pos;

	kt_stat_inc(kt_stat_trace_event);

	kt_clk_tick(&thr->clk, thr->id);

	trace = &thr->trace;
	clock = kt_clk_get(&thr->clk, thr->id);
	pos = clock % KT_TRACE_SIZE;

	if ((pos % KT_TRACE_PART_SIZE) == 0)
		kt_trace_switch(thr);

	event.type = type;
	event.data = data;
	BUG_ON(event.data != data);
	trace->events[pos] = event;
}

/* Same as kt_trace_add_event but saves 2 data items to trace.
 * Data is still stripped to 48 bits, but data2 is saved entirely.
 * The function ensures that the two words do not cross part boundary.
 * It is responsibility of kt_trace_follow to deal with both data items.
 */
void kt_trace_add_event2(kt_thr_t *thr, kt_event_type_t type, u64 data,
			 u64 data2);

/* Spinlock. */

void kt_spin_init(kt_spinlock_t *l);
void kt_spin_lock(kt_spinlock_t *l);
void kt_spin_unlock(kt_spinlock_t *l);
int kt_spin_is_locked(kt_spinlock_t *l);


void kt_shadow_clear(uptr_t addr, size_t size);

extern unsigned long kt_shadow_pages;

/* Threads. */

void kt_thr_pool_init(void);

kt_thr_t *kt_thr_create(kt_thr_t *thr, int pid);
void kt_thr_destroy(kt_thr_t *thr, kt_thr_t *old);
kt_thr_t *kt_thr_get(int id);

void kt_thr_start(kt_thr_t *thr, uptr_t pc);
void kt_thr_stop(kt_thr_t *thr, uptr_t pc);

void kt_thr_interrupt(kt_thr_t *thr, uptr_t pc, kt_interrupted_t *state);
void kt_thr_resume(kt_thr_t *thr, uptr_t pc, kt_interrupted_t *state);

bool kt_thr_event_disable(kt_thr_t *thr, uptr_t pc, unsigned long *flags);
bool kt_thr_event_enable(kt_thr_t *thr, uptr_t pc, unsigned long *flags);
void kt_thr_report_disable(kt_thr_t *thr);
void kt_thr_report_enable(kt_thr_t *thr);

/* Internal allocator. */

void kt_cache_init(kt_cache_t *cache, size_t obj_size, size_t obj_max_num);
void kt_cache_destroy(kt_cache_t *cache);
void *kt_cache_alloc(kt_cache_t *cache);
void kt_cache_free(kt_cache_t *cache, void *obj);

/*
 * Hash table. Maps an address to an arbitrary object.
 * The object must start with kt_tab_obj_t.
 */

void kt_tab_init(kt_tab_t *tab, unsigned size, unsigned obj_size,
		 unsigned obj_max_num);
void kt_tab_destroy(kt_tab_t *tab);
void *kt_tab_access(kt_tab_t *tab, uptr_t key, bool *created, bool destroy);

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

