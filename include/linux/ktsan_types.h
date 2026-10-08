/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A minimal header declaring types added by KTSAN to existing kernel structs.
 */
#ifndef _LINUX_KTSAN_TYPES_H
#define _LINUX_KTSAN_TYPES_H

struct kt_task_s;

/* Per-task KTSAN state, embedded into struct task_struct. */
struct ktsan_task_s {
	/*
	 * Runtime task state, NULL if the task is not tracked (created while
	 * KTSAN was disabled or the thread pool was exhausted).
	 */
	struct kt_task_s *task;
};

#endif /* _LINUX_KTSAN_TYPES_H */
