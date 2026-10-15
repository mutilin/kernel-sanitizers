// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

/*
 * TODO: port the reference kt_report_race(). It restores the racing thread's
 * stack and mutexset from its trace, which needs func entry/exit and lock
 * events in the trace first.
 *
 * Until locks and atomics are tracked, the only happens-before edge is
 * fork, so nearly every cross-thread alloc/free pair would show up as a
 * race. Races are only counted for now.
 */
void kt_report_race(kt_thr_t *new, kt_race_info_t *info)
{
	kt_stat_inc(kt_stat_reports);
}
