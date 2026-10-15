# SPDX-License-Identifier: GPL-2.0
obj-y        :=	init.o \
		shadow.o \
		spinlock.o \
		cache.o \
		tab.o \
		thr.o \
		stack_depot.o \
		clk.o \
		trace.o \
		access.o \
		memblock.o \
		sync.o \
		report.o \
		instrumentation.o
  
CFLAGS_REMOVE_init.o			:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_shadow.o			:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_cache.o			:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_tab.o			:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_thr.o			:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_stack_depot.o		:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_clk.o			:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_trace.o			:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_access.o			:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_memblock.o		:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_sync.o			:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_report.o			:= -pg -O0 -O1 -O2
CFLAGS_REMOVE_instrumentation.o		:= -pg -O0 -O1 -O2

CFLAGS_init.o			:= -O3 -fomit-frame-pointer
CFLAGS_shadow.o			:= -O3 -fomit-frame-pointer
CFLAGS_spinlock.o		:= -O3 -fomit-frame-pointer
CFLAGS_cache.o			:= -O3 -fomit-frame-pointer
CFLAGS_tab.o			:= -O3 -fomit-frame-pointer
CFLAGS_thr.o			:= -O3 -fomit-frame-pointer
CFLAGS_stack_depot.o		:= -O3 -fomit-frame-pointer
CFLAGS_clk.o			:= -O3 -fomit-frame-pointer
CFLAGS_trace.o			:= -O3 -fomit-frame-pointer
CFLAGS_access.o			:= -O3 -fomit-frame-pointer
CFLAGS_memblock.o		:= -O3 -fomit-frame-pointer
CFLAGS_sync.o			:= -O3 -fomit-frame-pointer
CFLAGS_report.o			:= -O3 -fomit-frame-pointer
CFLAGS_instrumentation.o	:= -O3 -fomit-frame-pointer

KTSAN_SANITIZE := n
KTSAN_SANITIZE_tests_inst.o = y
KCOV_INSTRUMENT := n
KASAN_SANITIZE := n
KCSAN_SANITIZE := n
KMSAN_SANITIZE := n
UBSAN_SANITIZE := n
