/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_MODES_H
#define KERNEL_SCHEDULER_MODES_H


#include <kscheduler.h>
#include <thread_types.h>


struct scheduler_mode_operations {
	const char*				name;

	bigtime_t				base_quantum __attribute__((aligned(8)));
	bigtime_t				minimal_quantum __attribute__((aligned(8)));
	bigtime_t				quantum_multipliers[2] __attribute__((aligned(8)));

	bigtime_t				maximum_latency __attribute__((aligned(8)));

	void					(*switch_to_mode)();
	void					(*set_cpu_enabled)(int32 cpu, bool enabled);
	bool					(*has_cache_expired)(
								const Scheduler::ThreadData* threadData, bigtime_t now);
	Scheduler::CoreEntry*	(*choose_core)(
								const Scheduler::ThreadData* threadData,
								const CPUSet& mask, bigtime_t now);
	Scheduler::CoreEntry*	(*rebalance)(
								const Scheduler::ThreadData* threadData,
								const CPUSet& mask, bigtime_t now);
	void					(*rebalance_irqs)(bool idle);
};

extern struct scheduler_mode_operations gSchedulerLowLatencyMode;
extern struct scheduler_mode_operations gSchedulerPowerSavingMode;


namespace Scheduler {

class ThreadData;
class CoreEntry;

}


#endif	// KERNEL_SCHEDULER_MODES_H

