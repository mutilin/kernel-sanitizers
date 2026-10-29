// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static const struct {
	kt_stat_t i;
	const char *s;
} desc[] = {
	{ kt_stat_reports, "reports" },
	{ kt_stat_races, "races" },
	{ kt_stat_access_read, "access_read" },
	{ kt_stat_access_write, "access_write" },
	{ kt_stat_access_size1, "access_size1" },
	{ kt_stat_access_size2, "access_size2" },
	{ kt_stat_access_size4, "access_size4" },
	{ kt_stat_access_size8, "access_size8" },
	{ kt_stat_sync_objects, "sync_objects" },
	{ kt_stat_sync_alloc, "sync_alloc" },
	{ kt_stat_sync_free, "sync_free" },
	{ kt_stat_memblock_objects, "memblock_objects" },
	{ kt_stat_memblock_alloc, "memblock_alloc" },
	{ kt_stat_memblock_free, "memblock_free" },
	{ kt_stat_threads, "threads" },
	{ kt_stat_thread_create, "thread_create" },
	{ kt_stat_thread_destroy, "thread_destroy" },
	{ kt_stat_acquire, "acquire" },
	{ kt_stat_release, "release" },
	{ kt_stat_func_entry, "func_entry" },
	{ kt_stat_func_exit, "func_exit" },
	{ kt_stat_trace_event, "trace_event" },
};

void kt_stat_collect(kt_stats_t *stat)
{
	kt_stats_t *stat1;
	int cpu, i;

	memset(stat, 0, sizeof(*stat));
	for_each_possible_cpu(cpu) {
		stat1 = &per_cpu_ptr(kt_ctx.cpus, cpu)->stat;
		for (i = 0; i < kt_stat_count; i++)
			stat->stat[i] += READ_ONCE(stat1->stat[i]);
	}
}

static int kt_stat_show(struct seq_file *m, void *v)
{
	unsigned long depot_nstacks, depot_memory;
	kt_stats_t stat;
	int i;

	if (!kt_ctx.cpus)
		return 0;

	kt_stat_collect(&stat);
	for (i = 0; i < ARRAY_SIZE(desc); i++)
		seq_printf(m, "%s: %lu\n", desc[i].s, stat.stat[desc[i].i]);
	kt_stack_depot_stats(&kt_ctx.stack_depot, &depot_nstacks,
			     &depot_memory);
	seq_printf(m, "depot_nstacks: %lu\n", depot_nstacks);
	seq_printf(m, "depot_memory_mb: %lu\n", depot_memory >> 20);
	return 0;
}

static int __init kt_stat_init(void)
{
	proc_create_single("ktsan_stats", 0400, NULL, kt_stat_show);
	return 0;
}
late_initcall(kt_stat_init);
