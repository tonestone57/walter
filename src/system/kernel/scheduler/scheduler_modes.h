/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_MODES_H
#define KERNEL_SCHEDULER_MODES_H

#include <kscheduler.h>
#include <thread_types.h>

enum { SCHEDULER_QUANTUM_INTERACTIVE = 0, SCHEDULER_QUANTUM_BACKGROUND = 1 };

struct scheduler_mode_operations {
	const char* name;

	// Configuration constants grouped to stay in one cache line
	bigtime_t base_quantum;
	bigtime_t minimal_quantum;
	bigtime_t quantum_multipliers[2];
	bigtime_t maximum_latency;

	// Dispatch table
	void (*switch_to_mode)();
	void (*set_cpu_enabled)(int32 cpu, bool enabled);
	bool (*has_cache_expired)(const Scheduler::ThreadData* threadData,
							  bigtime_t now);
	void (*update_thread_timeslice)(Scheduler::ThreadData* threadData);
	Scheduler::CoreEntry* (*choose_core)(
		const Scheduler::ThreadData* threadData, const CPUSet& mask,
		bigtime_t now);
	Scheduler::CoreEntry* (*rebalance)(const Scheduler::ThreadData* threadData,
									   const CPUSet& mask, bigtime_t now);
	void (*rebalance_irqs)(bool idle);
};

extern struct scheduler_mode_operations gSchedulerLowLatencyMode;
extern struct scheduler_mode_operations gSchedulerPowerSavingMode;

namespace Scheduler {

class ThreadData;
class CoreEntry;

}  // namespace Scheduler

#endif	// KERNEL_SCHEDULER_MODES_H
