/*
 * Copyright 2003-2004, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_RISCV64_CPU_H
#define _KERNEL_ARCH_RISCV64_CPU_H


#include <arch/riscv64/arch_thread_types.h>
#include <arch_cpu_defs.h>
#include <kernel.h>


#define CPU_MAX_CACHE_LEVEL	8
#define CACHE_LINE_SIZE		64


static inline void
arch_cpu_enable_user_access()
{
	SetBitsSstatus(SstatusReg{.sum = 1}.val);
}


static inline void
arch_cpu_disable_user_access()
{
	ClearBitsSstatus(SstatusReg{.sum = 1}.val);
}


typedef struct arch_cpu_info {
	uint64 hartId;
} arch_cpu_info;


#ifdef __cplusplus
extern "C" {
#endif


void __riscv64_setup_system_time(uint64 conversionFactor);


static inline void
arch_cpu_pause(void)
{
	// RISC-V Zihintpause extension hint encoded as FENCE with pred=W, succ=0
	// (0x0100000F). Recognized as a hint by cores supporting pause, and
	// safely executed as a nop/fence on older RISC-V cores.
	__asm__ __volatile__(".insn i 0x0F, 0, x0, x0, 0x010" : : : "memory");
}


static inline void
arch_cpu_idle(void)
{
	Wfi();
}


#ifdef __cplusplus
}
#endif


#endif	/* _KERNEL_ARCH_RISCV64_CPU_H */
