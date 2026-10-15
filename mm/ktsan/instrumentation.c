// SPDX-License-Identifier: GPL-2.0
/*
 * Compiler instrumentation ABI: the __tsan_*() callbacks that clang emits for
 * code built with -fsanitize=thread (flags in scripts/Makefile.ktsan).
 *
 * The callback set and signatures follow KCSAN's kernel/kcsan/core.c, which
 * implements the same ABI for this compiler.
 *
 * For now the callbacks only let the instrumented kernel link and boot: the
 * access, volatile and func entry/exit callbacks are empty, and atomics and
 * memory intrinsics just perform the operation.
 * TODO: route them to the runtime (kt_access*(), shadow stack) once the
 * trace and stack pieces are ported. They will then also need
 * user_access_save()/user_access_restore(), as in KMSAN: the compiler calls
 * them inside user_access_begin() regions, with the SMAP AC flag set.
 */
#include "ktsan.h"

#include <linux/export.h>
#include <linux/string.h>

/* Plain accesses; a compound read-write is one call. */

#define DEFINE_TSAN_READ_WRITE(size)                                          \
	void __tsan_read##size(void *ptr);                                    \
	void __tsan_read##size(void *ptr)                                     \
	{                                                                     \
	}                                                                     \
	EXPORT_SYMBOL(__tsan_read##size);                                     \
	void __tsan_unaligned_read##size(void *ptr)                           \
		__alias(__tsan_read##size);                                   \
	EXPORT_SYMBOL(__tsan_unaligned_read##size);                           \
	void __tsan_write##size(void *ptr);                                   \
	void __tsan_write##size(void *ptr)                                    \
	{                                                                     \
	}                                                                     \
	EXPORT_SYMBOL(__tsan_write##size);                                    \
	void __tsan_unaligned_write##size(void *ptr)                          \
		__alias(__tsan_write##size);                                  \
	EXPORT_SYMBOL(__tsan_unaligned_write##size);                          \
	void __tsan_read_write##size(void *ptr);                              \
	void __tsan_read_write##size(void *ptr)                               \
	{                                                                     \
	}                                                                     \
	EXPORT_SYMBOL(__tsan_read_write##size);                               \
	void __tsan_unaligned_read_write##size(void *ptr)                     \
		__alias(__tsan_read_write##size);                             \
	EXPORT_SYMBOL(__tsan_unaligned_read_write##size)

DEFINE_TSAN_READ_WRITE(1);
DEFINE_TSAN_READ_WRITE(2);
DEFINE_TSAN_READ_WRITE(4);
DEFINE_TSAN_READ_WRITE(8);
DEFINE_TSAN_READ_WRITE(16);

void __tsan_read_range(void *ptr, size_t size);
void __tsan_read_range(void *ptr, size_t size)
{
}
EXPORT_SYMBOL(__tsan_read_range);

void __tsan_write_range(void *ptr, size_t size);
void __tsan_write_range(void *ptr, size_t size)
{
}
EXPORT_SYMBOL(__tsan_write_range);

/* Volatile accesses: READ_ONCE()/WRITE_ONCE() and friends. */

#define DEFINE_TSAN_VOLATILE_READ_WRITE(size)                                 \
	void __tsan_volatile_read##size(void *ptr);                           \
	void __tsan_volatile_read##size(void *ptr)                            \
	{                                                                     \
	}                                                                     \
	EXPORT_SYMBOL(__tsan_volatile_read##size);                            \
	void __tsan_unaligned_volatile_read##size(void *ptr)                  \
		__alias(__tsan_volatile_read##size);                          \
	EXPORT_SYMBOL(__tsan_unaligned_volatile_read##size);                  \
	void __tsan_volatile_write##size(void *ptr);                          \
	void __tsan_volatile_write##size(void *ptr)                           \
	{                                                                     \
	}                                                                     \
	EXPORT_SYMBOL(__tsan_volatile_write##size);                           \
	void __tsan_unaligned_volatile_write##size(void *ptr)                 \
		__alias(__tsan_volatile_write##size);                         \
	EXPORT_SYMBOL(__tsan_unaligned_volatile_write##size)

DEFINE_TSAN_VOLATILE_READ_WRITE(1);
DEFINE_TSAN_VOLATILE_READ_WRITE(2);
DEFINE_TSAN_VOLATILE_READ_WRITE(4);
DEFINE_TSAN_VOLATILE_READ_WRITE(8);
DEFINE_TSAN_VOLATILE_READ_WRITE(16);

/* Function entry/exit; @call_pc is the return address into the caller. */

void __tsan_func_entry(void *call_pc);
void __tsan_func_entry(void *call_pc)
{
}
EXPORT_SYMBOL(__tsan_func_entry);

void __tsan_func_exit(void);
void __tsan_func_exit(void)
{
}
EXPORT_SYMBOL(__tsan_func_exit);

/* Called from the module constructor that -fsanitize=thread emits. */
void __tsan_init(void);
void __tsan_init(void)
{
}
EXPORT_SYMBOL(__tsan_init);

/*
 * Atomic builtins (__atomic_*, __sync_*). The callback must perform the
 * operation itself. This file is not instrumented, so the builtins below
 * compile to the real atomic instructions.
 */

#define DEFINE_TSAN_ATOMIC_LOAD_STORE(bits)                                   \
	u##bits __tsan_atomic##bits##_load(const u##bits *ptr, int memorder); \
	u##bits __tsan_atomic##bits##_load(const u##bits *ptr, int memorder)  \
	{                                                                     \
		return __atomic_load_n(ptr, memorder);                        \
	}                                                                     \
	EXPORT_SYMBOL(__tsan_atomic##bits##_load);                            \
	void __tsan_atomic##bits##_store(u##bits *ptr, u##bits v,             \
					 int memorder);                       \
	void __tsan_atomic##bits##_store(u##bits *ptr, u##bits v,             \
					 int memorder)                        \
	{                                                                     \
		__atomic_store_n(ptr, v, memorder);                           \
	}                                                                     \
	EXPORT_SYMBOL(__tsan_atomic##bits##_store)

#define DEFINE_TSAN_ATOMIC_RMW(op, bits, suffix)                              \
	u##bits __tsan_atomic##bits##_##op(u##bits *ptr, u##bits v,           \
					   int memorder);                     \
	u##bits __tsan_atomic##bits##_##op(u##bits *ptr, u##bits v,           \
					   int memorder)                      \
	{                                                                     \
		return __atomic_##op##suffix(ptr, v, memorder);               \
	}                                                                     \
	EXPORT_SYMBOL(__tsan_atomic##bits##_##op)

#define DEFINE_TSAN_ATOMIC_CMPXCHG(bits, strength, weak)                      \
	int __tsan_atomic##bits##_compare_exchange_##strength(                \
		u##bits *ptr, u##bits *exp, u##bits val, int mo,              \
		int fail_mo);                                                 \
	int __tsan_atomic##bits##_compare_exchange_##strength(                \
		u##bits *ptr, u##bits *exp, u##bits val, int mo, int fail_mo) \
	{                                                                     \
		return __atomic_compare_exchange_n(ptr, exp, val, weak, mo,   \
						   fail_mo);                  \
	}                                                                     \
	EXPORT_SYMBOL(__tsan_atomic##bits##_compare_exchange_##strength)

#define DEFINE_TSAN_ATOMIC_CMPXCHG_VAL(bits)                                  \
	u##bits __tsan_atomic##bits##_compare_exchange_val(                   \
		u##bits *ptr, u##bits exp, u##bits val, int mo, int fail_mo); \
	u##bits __tsan_atomic##bits##_compare_exchange_val(                   \
		u##bits *ptr, u##bits exp, u##bits val, int mo, int fail_mo)  \
	{                                                                     \
		__atomic_compare_exchange_n(ptr, &exp, val, 0, mo, fail_mo);  \
		return exp;                                                   \
	}                                                                     \
	EXPORT_SYMBOL(__tsan_atomic##bits##_compare_exchange_val)

#define DEFINE_TSAN_ATOMIC_OPS(bits)                                          \
	DEFINE_TSAN_ATOMIC_LOAD_STORE(bits);                                  \
	DEFINE_TSAN_ATOMIC_RMW(exchange, bits, _n);                           \
	DEFINE_TSAN_ATOMIC_RMW(fetch_add, bits, );                            \
	DEFINE_TSAN_ATOMIC_RMW(fetch_sub, bits, );                            \
	DEFINE_TSAN_ATOMIC_RMW(fetch_and, bits, );                            \
	DEFINE_TSAN_ATOMIC_RMW(fetch_or, bits, );                             \
	DEFINE_TSAN_ATOMIC_RMW(fetch_xor, bits, );                            \
	DEFINE_TSAN_ATOMIC_RMW(fetch_nand, bits, );                           \
	DEFINE_TSAN_ATOMIC_CMPXCHG(bits, strong, 0);                          \
	DEFINE_TSAN_ATOMIC_CMPXCHG(bits, weak, 1);                            \
	DEFINE_TSAN_ATOMIC_CMPXCHG_VAL(bits)

DEFINE_TSAN_ATOMIC_OPS(8);
DEFINE_TSAN_ATOMIC_OPS(16);
DEFINE_TSAN_ATOMIC_OPS(32);
DEFINE_TSAN_ATOMIC_OPS(64);

void __tsan_atomic_thread_fence(int memorder);
void __tsan_atomic_thread_fence(int memorder)
{
	__atomic_thread_fence(memorder);
}
EXPORT_SYMBOL(__tsan_atomic_thread_fence);

void __tsan_atomic_signal_fence(int memorder);
void __tsan_atomic_signal_fence(int memorder)
{
	__atomic_signal_fence(memorder);
}
EXPORT_SYMBOL(__tsan_atomic_signal_fence);

/*
 * Memory intrinsics: the instrumentation turns memcpy()/memset()/memmove()
 * in instrumented code into these. The real (assembly) versions do the work.
 */
void *__tsan_memset(void *s, int c, size_t count);
noinline void *__tsan_memset(void *s, int c, size_t count)
{
	return memset(s, c, count);
}
EXPORT_SYMBOL(__tsan_memset);

void *__tsan_memmove(void *dst, const void *src, size_t len);
noinline void *__tsan_memmove(void *dst, const void *src, size_t len)
{
	return memmove(dst, src, len);
}
EXPORT_SYMBOL(__tsan_memmove);

void *__tsan_memcpy(void *dst, const void *src, size_t len);
noinline void *__tsan_memcpy(void *dst, const void *src, size_t len)
{
	return memcpy(dst, src, len);
}
EXPORT_SYMBOL(__tsan_memcpy);
