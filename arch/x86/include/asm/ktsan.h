/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_KTSAN_H
#define _ASM_X86_KTSAN_H

#ifndef MODULE

#include <asm/cpu_entry_area.h>
#include <asm/processor.h>
#include <linux/mmzone.h>

/*
 * Taken from arch/x86/mm/physaddr.h to avoid using an instrumented version.
 */
static inline bool ktsan_phys_addr_valid(unsigned long addr)
{
	if (IS_ENABLED(CONFIG_PHYS_ADDR_T_64BIT))
		return !(addr >> boot_cpu_data.x86_phys_bits);
	else
		return true;
}

/*
 * Taken from arch/x86/mm/physaddr.c to avoid using an instrumented version.
 */
static inline bool ktsan_virt_addr_valid(void *addr)
{
	unsigned long x = (unsigned long)addr;
	unsigned long y = x - __START_KERNEL_map;
	bool ret;

	/* use the carry flag to determine if x was < __START_KERNEL_map */
	if (unlikely(x > y)) {
		x = y + phys_base;

		if (y >= KERNEL_IMAGE_SIZE)
			return false;
	} else {
		x = y + (__START_KERNEL_map - PAGE_OFFSET);

		/* carry flag will be set if starting x was >= PAGE_OFFSET */
		if ((x > y) || !ktsan_phys_addr_valid(x))
			return false;
	}
	/*TODO*/
	/*
	 * pfn_valid() relies on RCU, and may call into the scheduler on exiting
	 * the critical section. However, this would result in recursion with
	 * KMSAN. Therefore, disable preemption here, and re-enable preemption
	 * below while suppressing reschedules to avoid recursion.
	 *
	 * Note, this sacrifices occasionally breaking scheduling guarantees.
	 * Although, a kernel compiled with KMSAN has already given up on any
	 * performance guarantees due to being heavily instrumented.
	 */
	preempt_disable();
	ret = pfn_valid(x >> PAGE_SHIFT);
	preempt_enable_no_resched();

	return ret;
}

#endif /* !MODULE */
#endif /* _ASM_X86_KTSAN_H */ 
