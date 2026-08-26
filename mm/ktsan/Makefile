# SPDX-License-Identifier: GPL-2.0
obj-y        :=	init.o \
		shadow.o \
	 
  
CFLAGS_REMOVE_init.o			= -pg -O0 -O1 -O2
CFLAGS_REMOVE_shadow.o			= -pg -O0 -O1 -O2

CFLAGS_init.o			= -O3 -fomit-frame-pointer
CFLAGS_shadow.o			= -O3 -fomit-frame-pointer

KTSAN_SANITIZE := n
KTSAN_SANITIZE_tests_inst.o = y
KCOV_INSTRUMENT := n
