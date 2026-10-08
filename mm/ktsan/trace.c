// SPDX-License-Identifier: GPL-2.0
#include "ktsan.h"

#include <linux/kernel.h>

void kt_trace_init(kt_trace_t *trace)
{
	int i;

	memset(trace, 0, sizeof(*trace));
	for (i = 0; i < KT_TRACE_PARTS; i++)
		trace->headers[i].state.cpu_id = -1;
	kt_spin_init(&trace->lock);
}
