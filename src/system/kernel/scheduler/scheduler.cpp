/*
 * Copyright 2013-2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2009, Rene Gollent, rene@gollent.com.
 * Copyright 2008-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2010, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2002, Angelo Mottola, a.mottola@libero.it.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */

#include <OS.h>
#include <AutoDeleter.h>
#include <cpu.h>
#include <debug.h>
#include <stdint.h>
#include <kernel.h>
#include <kscheduler.h>
#include <listeners.h>
#include <load_tracking.h>
#include <smp.h>
#include <timer.h>
#include <util/Random.h>
#include <util/DoublyLinkedList.h>
#include <algorithm>
#include <math.h> // For roundf
#include <thread.h>
#include <kernel/thread.h>

#include <stdlib.h> // For strtoul
#include <stdio.h>  // For kprintf, snprintf (though kprintf is kernel specific)


#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_defs.h"
#include "scheduler_locking.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"
#include "scheduler_tracing.h"

#include "scheduler_weights.h"
#include "EevdfScheduler.h"
#include "ThreadData.h"
#include <thread_defs.h>

#include <util/MultiHashTable.h>
#include <kernel/thread.h>
#include <syscalls.h>
#include <cstdint>
#include <kernel/thread.h>

typedef BOpenHashTable<Scheduler::IntHashDefinition> IRQAffinityMap;
static IRQAffinityMap* sIrqTaskAffinityMap;
static spinlock gIrqTaskAffinityLock;


/*! The thread scheduler */

// Define missing DEFAULT_... macros with values from comments
#define DEFAULT_IRQ_BALANCE_CHECK_INTERVAL 500000 // Default 0.5s, assuming microseconds
#define DEFAULT_IRQ_TARGET_FACTOR 0.3f
#define DEFAULT_MAX_TARGET_CPU_IRQ_LOAD 700
#define DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD 1000
#define DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE 300
#define DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY 3

// EEVDF Specific Defines (Initial values, require tuning)
// SCHEDULER_WEIGHT_SCALE is now defined in src/system/kernel/scheduler/scheduler_defs.h

namespace Scheduler {
}

// --- New Continuous Weight Calculation Logic ---

// Minimum and maximum weights for the new scheme
static const int32 kNewMinActiveWeight = 15; // Similar to current gNiceToWeight[39], a floor for active threads.
static const int32 kNewMaxWeightCap = 35000000;

// Removido: gHaikuContinuousWeights, _init_continuous_weights, kUseContinuousWeights
// A lógica de peso agora está encapsulada em scheduler_weights.cpp.


static int
cmd_thread_sched_info(int argc, char** argv)
{
	if (argc != 2) {
		kprintf("Usage: thread_sched_info <thread_id>\n");
		return B_KDEBUG_ERROR;
	}

	thread_id id = strtoul(argv[1], NULL, 0);
	if (id <= 0) {
		kprintf("Invalid thread ID: %s\n", argv[1]);
		return B_KDEBUG_ERROR;
	}

	Thread* thread = Thread::Get(id);
	if (thread == NULL) {
		kprintf("Thread %" B_PRId32 " not found.\n", id);
		return B_KDEBUG_ERROR;
	}
	BReference<Thread> threadRef(thread, true);

	thread->Lock();
	InterruptsSpinLocker schedulerLocker(thread->scheduler_lock);

	kprintf("Scheduler Info for Thread %" B_PRId32 " (\"%s\"):\n", thread->id, thread->name);
	kprintf("--------------------------------------------------\n");
	kprintf("Base Priority:      %" B_PRId32 "\n", thread->priority);

	if (thread->scheduler_data != NULL) {
		Scheduler::ThreadData* td = thread->scheduler_data;
		kprintf("Scheduler Data (ThreadData*) at: %p\n", td);
		td->Dump();

		kprintf("\nAdditional Scheduler Details:\n");
		kprintf("  Pinned to CPU:      ");
		if (thread->pinned_to_cpu > 0) {
			kprintf("%" B_PRId32 "\n", thread->pinned_to_cpu - 1);
		} else {
			kprintf("no\n");
		}
		kprintf("  CPU Affinity Mask:  ");
		CPUSet affinityMask = td->GetCPUMask();
		bool allSet = true;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (!affinityMask.GetBit(i)) {
				allSet = false;
				break;
			}
		}
		if (affinityMask.IsEmpty() || allSet) {
			kprintf("%s\n", affinityMask.IsEmpty() ? "none" : "all");
		} else {
			kprintf("0x");
			for (int32 i = (smp_get_num_cpus() + 31) / 32 - 1; i >= 0; i--) {
				if (affinityMask.Bits(i) != 0)
					kprintf("%x", affinityMask.Bits(i));
			}
			uint32 count = 0;
			for (int32 i = 0; i < smp_get_num_cpus(); i++) {
				if (affinityMask.GetBit(i))
					count++;
			}
			kprintf(" (%" B_PRIu32 " bits set)\n", count);
		}

		kprintf("  I/O Bound Heuristic:\n");
		kprintf("    Avg Run Burst (us): %" B_PRId64 "\n", td->AverageRunBurstTime());
		kprintf("    Voluntary Sleeps:   %" B_PRIu32 "\n", td->VoluntarySleepTransitions());
		kprintf("    Is Likely I/O Bound: %s\n", td->IsLikelyIOBound() ? "yes" : "no");

		kprintf("  Affinitized IRQs:\n");
		int8 irqCount = 0;
		const int32* affIrqs = td->GetAffinitizedIrqs(irqCount);
		if (irqCount > 0) {
			for (int8 i = 0; i < irqCount; ++i) {
				kprintf("    IRQ %" B_PRId32 "\n", affIrqs[i]);
			}
		} else {
			kprintf("    none\n");
		}

	} else {
		kprintf("Scheduler Data:     <not initialized/available>\n");
	}

	schedulerLocker.Unlock();

	thread->Unlock();

	kprintf("--------------------------------------------------\n");
	return 0;
}

namespace Scheduler {





class ThreadEnqueuer : public ThreadProcessing {
public:
	void		operator()(ThreadData* thread);
};

scheduler_mode gCurrentModeID;
scheduler_mode_operations* gCurrentMode;

CPUSet gCPUEnabled;
bool gSingleCore;
bool gTrackCoreLoad;
bool gTrackCPULoad;
// float gKernelKDistFactor = DEFAULT_K_DIST_FACTOR; // REMOVED

SchedulerLoadBalancePolicy gSchedulerLoadBalancePolicy = Scheduler::SPREAD;
float gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;

bigtime_t gIRQBalanceCheckInterval = DEFAULT_IRQ_BALANCE_CHECK_INTERVAL;
float gModeIrqTargetFactor = DEFAULT_IRQ_TARGET_FACTOR;
int32 gModeMaxTargetCpuIrqLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD;
int32 gHighAbsoluteIrqThreshold = DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD;
int32 gSignificantIrqLoadDifference = DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE;
int32 gMaxIRQsToMoveProactively = DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY;

static const bigtime_t kIrqFollowTaskCooldownPeriod = 50000;
static int64 gIrqLastFollowMoveTime[NUM_IO_VECTORS];


}	// namespace Scheduler



status_t
_user_set_scheduler_mode(int32 mode)
{
	return scheduler_set_operation_mode((scheduler_mode)mode);
}


int32
_user_get_scheduler_mode(void)
{
	return Scheduler::gCurrentModeID;
}



using namespace Scheduler;

static bool sSchedulerEnabled;
SchedulerListenerList gSchedulerListeners;
static spinlock sSchedulerListenersLock = B_SPINLOCK_INITIALIZER;

static scheduler_mode_operations* sSchedulerModes[] = {
	&gSchedulerLowLatencyMode,
	&gSchedulerPowerSavingMode,
};

static int32* sCPUToCore;
static int32* sCPUToPackage;

static inline bigtime_t
scheduler_calculate_eevdf_slice(Scheduler::ThreadData* threadData, const Scheduler::CPUEntry* cpu)
{
	if (threadData == NULL) return kMinSliceGranularity;
	return threadData->CalculateDynamicQuantum(cpu);
}

static bool scheduler_perform_load_balance(void);
static int32 scheduler_load_balance_event(timer* unused);

void
scheduler_maybe_follow_task_irqs(thread_id thId, const int32* irqList,
	int8 irqCount, CoreEntry* targetCore, Scheduler::CPUEntry* targetCPU)
{
}
static timer sIRQBalanceTimer;
static int32 scheduler_irq_balance_event(timer* unused);
static Scheduler::CPUEntry* _scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqVector, int32 irqToMoveLoad);
static Scheduler::CPUEntry* _scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest, const Scheduler::ThreadData* affinityCheckThread);

static int cmd_scheduler_set_smt_factor(int argc, char** argv);
static int cmd_scheduler_get_smt_factor(int argc, char** argv);

// static Scheduler::CPUEntry* find_quiet_cpu_for_irq(irq_assignment* irq, Scheduler::CPUEntry* current);


static Scheduler::CPUEntry*
_find_idle_cpu_on_core(CoreEntry* core)
{
	if (core == NULL || core->IsDefunct()) return NULL;
	CPUSet coreCPUs = core->CPUMask();
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (coreCPUs.GetBit(i) && !gCPU[i].disabled) {
			Thread* runningThread = gCPU[i].running_thread;
			if (runningThread != NULL && runningThread->scheduler_data != NULL
				&& runningThread->scheduler_data->IsIdle()) {
				return Scheduler::CPUEntry::GetCPU(i);
			}
		}
	}
	return NULL;
}

static timer sLoadBalanceTimer;
static bigtime_t gDynamicLoadBalanceInterval = kInitialLoadBalanceInterval;
// minimum time between two migrations of the same thread
static const bigtime_t kMinTimeBetweenMigrations = 10000;
// penalty factor for I/O bound threads
static const int32 kIOBoundScorePenaltyFactor = 4;
// factor for lag in benefit score
static const int32 kBenefitScoreLagFactor = 2;
// factor for eligibility improvement in benefit score
static const int32 kBenefitScoreEligFactor = 1;
// minimum unweighted normalized work to steal
static const bigtime_t kMinUnweightedNormWorkToSteal = 1000;



void
scheduler_dump_thread_data(Thread* thread)
{
	thread->scheduler_data->Dump();
}


void
scheduler_enqueue_in_run_queue(Thread* thread)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(thread != NULL);
	Scheduler::ThreadData* data = thread->scheduler_data;
	if (!data)
		return;

	Scheduler::CPUEntry* cpu = NULL;
	Scheduler::CoreEntry* core = NULL;
	data->ChooseCoreAndCPU(core, cpu);
	ASSERT(cpu != NULL && core != NULL);

	InterruptsSpinLocker locker(thread->scheduler_lock);

	if (data->IsIdle()) {
		if (thread->state != B_THREAD_RUNNING)
			thread->state = B_THREAD_READY;
		return;
	}

	data->UpdateEevdfParameters(cpu, true, false);
	data->MarkEnqueued(core);

	cpu->LockRunQueue();
	cpu->GetEevdfScheduler().AddThread(data);
	cpu->UnlockRunQueue();

	Thread* running = gCPU[cpu->ID()].running_thread;
	if (!running || thread_is_idle_thread(running)) {
		gCPU[cpu->ID()].invoke_scheduler = true;
	} else {
		Scheduler::ThreadData* runningData = running->scheduler_data;
		if (system_time() >= data->EligibleTime() &&
			data->VirtualDeadline() < runningData->VirtualDeadline()) {
			smp_send_ici(cpu->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	}
}




// Sets the priority of a thread.
// This function must be called with interrupts enabled.
int32
scheduler_set_thread_priority(Thread *thread, int32 priority)
{
	ASSERT(are_interrupts_enabled());
	InterruptsSpinLocker interruptLocker(thread->scheduler_lock);
	SCHEDULER_ENTER_FUNCTION();

	Scheduler::ThreadData* threadData = thread->scheduler_data;
	int32 oldActualPriority = thread->priority;

	TRACE_SCHED("scheduler_set_thread_priority (EEVDF): T %" B_PRId32 " from prio %" B_PRId32 " to %" B_PRId32 "\n",
		thread->id, oldActualPriority, priority);

	Scheduler::CPUEntry* cpuContextForUpdate = NULL;
	bool wasRunning = (thread->state == B_THREAD_RUNNING && thread->cpu != NULL);
	bool wasReadyAndEnqueuedPrior = (thread->state == B_THREAD_READY && threadData->IsEnqueued());

	if (wasRunning) {
		cpuContextForUpdate = Scheduler::CPUEntry::GetCPU(thread->cpu->cpu_num);
	} else if (wasReadyAndEnqueuedPrior) {
		if (thread->previous_cpu != NULL && threadData->Core() != NULL
			&& Scheduler::CPUEntry::GetCPU(thread->previous_cpu->cpu_num)->Core() == threadData->Core()) {
			cpuContextForUpdate = Scheduler::CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
		} else if (threadData->Core() != NULL && threadData->Core()->CPUCount() > 0) {
			int32 firstCpuIdOnCore = -1;
			for (int32 i = 0; i < smp_get_num_cpus(); i++) {
				if (threadData->Core()->CPUMask().GetBit(i)) {
					firstCpuIdOnCore = i;
					break;
				}
			}
			if (firstCpuIdOnCore >= 0)
				cpuContextForUpdate = Scheduler::CPUEntry::GetCPU(firstCpuIdOnCore);
			TRACE_SCHED("set_prio: T %" B_PRId32 " ready&enqueued, using first CPU (%d) of its core (%d) as context for weight calc.\n",
                thread->id, firstCpuIdOnCore, threadData->Core()->ID() );
		} else {
			TRACE_SCHED("set_prio: T %" B_PRId32 " ready&enqueued, but no valid CPU context found for weight calc. Using NULL.\n", thread->id);
		}
	}

	int32 oldWeight = scheduler_priority_to_weight(thread, cpuContextForUpdate);

	thread->priority = priority;
	int32 newWeight = scheduler_priority_to_weight(thread, cpuContextForUpdate);

	if (wasRunning) {
		ASSERT(cpuContextForUpdate != NULL);
	} else if (wasReadyAndEnqueuedPrior) {
		if (thread->previous_cpu != NULL && threadData->Core() != NULL
			&& Scheduler::CPUEntry::GetCPU(thread->previous_cpu->cpu_num)->Core() == threadData->Core()) {
			// cpuContextForUpdate already set
		} else if (threadData->Core() != NULL) {
			TRACE_SCHED("set_prio: T %" B_PRId32 " ready&enqueued, but previous_cpu inconsistent or NULL for oldWeight/newWeight context. Using potentially already set or new first CPU of core.\n", thread->id);
		}
	}


	if (cpuContextForUpdate != NULL && oldWeight != newWeight && newWeight > 0) {
		cpuContextForUpdate->LockRunQueue();
		bigtime_t min_v = cpuContextForUpdate->MinVirtualRuntime();
		bigtime_t currentVRuntime = threadData->VirtualRuntime();
		if (currentVRuntime > min_v) {
			bigtime_t delta_v = currentVRuntime - min_v;
			bigtime_t newAdjustedVRuntime = min_v + (delta_v * oldWeight) / newWeight;
			threadData->SetVirtualRuntime(newAdjustedVRuntime);
			TRACE_SCHED("set_prio: T %" B_PRId32 " vruntime adjusted from %" B_PRId64 " to %" B_PRId64 " (weight %" B_PRId32 "->%" B_PRId32 ") rel_to_min_v %" B_PRId64 "\n",
				thread->id, currentVRuntime, newAdjustedVRuntime, oldWeight, newWeight, min_v);
		}
		cpuContextForUpdate->UnlockRunQueue();
	}

	if (wasRunning && oldWeight != newWeight && oldWeight > 0 && newWeight > 0) {
		bigtime_t actualRuntimeInSlice = threadData->TimeUsedInCurrentQuantum();
		if (actualRuntimeInSlice > 0) {
			bigtime_t weightedRuntimeOld = (actualRuntimeInSlice * SCHEDULER_WEIGHT_SCALE) / oldWeight;
			bigtime_t weightedRuntimeNew = (actualRuntimeInSlice * SCHEDULER_WEIGHT_SCALE) / newWeight;
			bigtime_t lagAdjustment = weightedRuntimeOld - weightedRuntimeNew;

			threadData->AddLag(lagAdjustment);
			TRACE_SCHED("set_prio: T %" B_PRId32 " ran %" B_PRId64 "us in slice. Lag adjusted by %" B_PRId64 " due to weight change (%d->%d). New Lag before recalc: %" B_PRId64 "\n",
				thread->id, actualRuntimeInSlice, lagAdjustment, oldWeight, newWeight, threadData->Lag());
		}
	}

	threadData->UpdateEevdfParameters(cpuContextForUpdate, false, false);

	TRACE_SCHED("set_prio: T %" B_PRId32 " (after UpdateEevdfParameters) new slice %" B_PRId64 ", new lag %" B_PRId64 ", new elig %" B_PRId64 ", new VD %" B_PRId64 "\n",
		thread->id, threadData->SliceDuration(), threadData->Lag(), threadData->EligibleTime(), threadData->VirtualDeadline());

	if (wasRunning) {
		ASSERT(cpuContextForUpdate != NULL);
		gCPU[cpuContextForUpdate->ID()].invoke_scheduler = true;
		if (cpuContextForUpdate->ID() != smp_get_current_cpu()) {
			smp_send_ici(cpuContextForUpdate->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	} else if (wasReadyAndEnqueuedPrior) {
		if (cpuContextForUpdate != NULL) {
			cpuContextForUpdate->LockRunQueue();
			cpuContextForUpdate->GetEevdfScheduler().UpdateThread(threadData, 0);
			cpuContextForUpdate->UnlockRunQueue();
			Thread* currentOnThatCpu = gCPU[cpuContextForUpdate->ID()].running_thread;
			if (currentOnThatCpu == NULL || thread_is_idle_thread(currentOnThatCpu)
				|| (system_time() >= threadData->EligibleTime()
					&& threadData->VirtualDeadline() < currentOnThatCpu->scheduler_data->VirtualDeadline())) {
				if (cpuContextForUpdate->ID() == smp_get_current_cpu()) {
					gCPU[cpuContextForUpdate->ID()].invoke_scheduler = true;
				} else {
					smp_send_ici(cpuContextForUpdate->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
				}
			}
			TRACE_SCHED("set_prio: T %" B_PRId32 " updated in runqueue on CPU %" B_PRId32 "\n",
				thread->id, cpuContextForUpdate->ID());
		} else {
			TRACE_SCHED_WARNING("set_prio: T %" B_PRId32 " was ready&enqueued, but no valid CPU context. Runqueue update skipped. Thread may need re-enqueue if VD changed significantly.\n", thread->id);
		}
	}
	return oldActualPriority;
}


void
scheduler_reschedule_ici()
{
	get_cpu_struct()->invoke_scheduler = true;
}


static void
thread_resumes(Thread* thread)
{
	cpu_ent* cpu = thread->cpu;
	release_spinlock(&cpu->previous_thread->scheduler_lock);
	if ((thread->flags & THREAD_FLAGS_DEBUGGER_INSTALLED) != 0)
		user_debug_thread_scheduled(thread);
}


void
scheduler_new_thread_entry(Thread* thread)
{
	thread_resumes(thread);
	SpinLocker locker(thread->time_lock);
	thread->last_time = system_time();
}



static Scheduler::ThreadData*
_attempt_one_steal(Scheduler::CPUEntry* thiefCPU, int32 victimCpuID)
{
	Scheduler::CPUEntry* victimCPUEntry = Scheduler::CPUEntry::GetCPU(victimCpuID);

	if (gCPU[victimCpuID].disabled || victimCPUEntry == NULL)
		return NULL;
	if (system_time() < victimCPUEntry->fLastTimeTaskStolenFrom + kVictimStealCooldownPeriod)
		return NULL;
	if (victimCPUEntry->GetTotalThreadCount() <= 0)
		return NULL;

	TRACE_SCHED("WorkSteal: Thief CPU %" B_PRId32 " probing victim CPU %" B_PRId32 "\n", thiefCPU->ID(), victimCpuID);

	Scheduler::ThreadData* stolenTask = NULL;
	victimCPUEntry->LockRunQueue();
	EevdfScheduler& victimQueue = victimCPUEntry->GetEevdfScheduler();

	if (!victimQueue.IsEmpty()) {
		struct thread* candidateThread = victimQueue.PeekMinThread();
		if (candidateThread != NULL) {
			Scheduler::ThreadData* candidateTask = (Scheduler::ThreadData*)candidateThread->scheduler_data;
			Thread* candThread = candidateTask->GetThread();
			bool basicChecksPass = true;

			if (candThread->pinned_to_cpu != 0) {
				if ((candThread->pinned_to_cpu - 1) != thiefCPU->ID()) {
					basicChecksPass = false;
				}
			}
			if (basicChecksPass && !candidateTask->GetCPUMask().IsEmpty()) {
				if (!candidateTask->GetCPUMask().GetBit(thiefCPU->ID())) {
					basicChecksPass = false;
				}
			}

			int32 candidateWeight = scheduler_priority_to_weight(
				candidateTask->GetThread(), victimCPUEntry);
			if (candidateWeight <= 0)
				candidateWeight = 1;
			bigtime_t unweightedNormWorkOwed
				= (candidateTask->Lag() * candidateWeight) / SCHEDULER_WEIGHT_SCALE;

			bool isStarved = unweightedNormWorkOwed > kMinUnweightedNormWorkToSteal;

			if (isStarved) {
				TRACE_SCHED_BL_STEAL("  WorkSteal Eval: T%" B_PRId32 " considered"
					" starved (unweighted_owed %" B_PRId64 " > effective_threshold %"
					B_PRId64 "). Original Lag_weighted %" B_PRId64 ".\n",
					candidateTask->GetThread()->id, unweightedNormWorkOwed,
					kMinUnweightedNormWorkToSteal,
					candidateTask->Lag());
			}

			if (basicChecksPass && isStarved) {
				bool allowStealByBLPolicy = false;
				scheduler_core_type thiefCoreType = thiefCPU->Core()->Type();
				scheduler_core_type victimCoreType = victimCPUEntry->Core()->Type();

				bool isTaskPCritical = (candidateTask->GetBasePriority() >= B_URGENT_DISPLAY_PRIORITY
					|| candidateTask->GetLoad() > (kMaxLoad * 7 / 10));

				TRACE_SCHED_BL_STEAL("WorkSteal Eval: Thief C%d(T%d), Victim C%d(T%d), Task T% " B_PRId32 " (Pcrit %d, EPref %d, Load %" B_PRId32 ", Lag %" B_PRId64 ")\n",
					thiefCPU->Core()->ID(), thiefCoreType, victimCPUEntry->Core()->ID(), victimCoreType,
					candThread->id, isTaskPCritical, false, candidateTask->GetLoad(), candidateTask->Lag());

				if (thiefCoreType == CORE_TYPE_BIG || thiefCoreType == CORE_TYPE_UNIFORM_PERFORMANCE) {
					if (isTaskPCritical) {
						allowStealByBLPolicy = true;
						TRACE_SCHED_BL_STEAL("  Decision: BIG thief, P-Critical task. ALLOW steal.\n");
					} else {
						uint32 victimCapacity = victimCPUEntry->Core()->PerformanceCapacity();
						if (victimCapacity == 0) victimCapacity = SCHEDULER_NOMINAL_CAPACITY;
						int32 victimEffectiveVeryHighLoad = (int32)((uint64)kVeryHighLoad * victimCapacity / SCHEDULER_NOMINAL_CAPACITY);
						if (victimCPUEntry->GetLoad() > victimEffectiveVeryHighLoad) {
							allowStealByBLPolicy = true;
							TRACE_SCHED_BL_STEAL("  Decision: BIG thief, EPref/Flex task, victim C%d very overloaded. ALLOW steal.\n", victimCPUEntry->Core()->ID());
						} else {
							TRACE_SCHED_BL_STEAL("  Decision: BIG thief, EPref/Flex task, victim C%d not very overloaded. DENY steal.\n", victimCPUEntry->Core()->ID());
						}
					}
				} else {
					if (isTaskPCritical) {
						allowStealByBLPolicy = false;
						if (victimCoreType == CORE_TYPE_LITTLE && victimCPUEntry->GetLoad() > thiefCPU->Core()->GetLoad() + kLoadDifference) {
							allowStealByBLPolicy = true;
							TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task. Victim is overloaded LITTLE. ALLOW steal (rescue).\n");
						} else if (victimCoreType == CORE_TYPE_BIG || victimCoreType == CORE_TYPE_UNIFORM_PERFORMANCE) {
							bool allBigCoresSaturated = true;
							for (int32 coreIdx = 0; coreIdx < gCoreCount; coreIdx++) {
								CoreEntry* core = &gCoreEntries[coreIdx];
								if (core->IsDefunct() || !(core->Type() == CORE_TYPE_BIG || core->Type() == CORE_TYPE_UNIFORM_PERFORMANCE))
									continue;
								uint32 pCoreCapacity = core->PerformanceCapacity() > 0 ? core->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
								int32 pCoreHighLoadThreshold = kHighLoad * pCoreCapacity / SCHEDULER_NOMINAL_CAPACITY;
								if (core->GetLoad() < pCoreHighLoadThreshold) {
									allBigCoresSaturated = false;
									TRACE_SCHED_BL_STEAL("  Eval P-crit steal by E-core: P-Core %" B_PRId32 " (load %" B_PRId32 ") not saturated (threshold %" B_PRId32 ").\n",
										core->ID(), core->GetLoad(), pCoreHighLoadThreshold);
									break;
								}
							}

							if (allBigCoresSaturated) {
								uint32 thiefCapacity = thiefCPU->Core()->PerformanceCapacity();
								if (thiefCapacity == 0) thiefCapacity = SCHEDULER_NOMINAL_CAPACITY;
								int32 lightTaskLoadThreshold = (int32)((uint64)thiefCapacity * 20 / 100 * kMaxLoad / SCHEDULER_NOMINAL_CAPACITY);
								if (candidateTask->GetLoad() < lightTaskLoadThreshold) {
									allowStealByBLPolicy = true;
									TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task from P-core. All P-cores saturated AND task load %" B_PRId32 " is light for thief. ALLOW steal.\n", candidateTask->GetLoad());
								} else {
									TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task from P-core. All P-cores saturated BUT task load %" B_PRId32 " too high for LITTLE. DENY steal.\n", candidateTask->GetLoad());
								}
							} else {
								TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task from P-core. Not all P-cores saturated. DENY steal.\n");
							}
						} else {
							TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task from LITTLE victim. Conditions for rescue not met. DENY steal.\n");
						}
					} else {
						allowStealByBLPolicy = true;
						TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, EPref/Flex task. ALLOW steal.\n");
					}
				}

				if (allowStealByBLPolicy) {
					struct thread* stolenThread = victimQueue.PopMinThread();
					stolenTask = (Scheduler::ThreadData*)stolenThread->scheduler_data;
					victimCPUEntry->fLastTimeTaskStolenFrom = system_time();
					int32 threadCount = victimCPUEntry->GetTotalThreadCount();
					atomic_add(&threadCount, -1);
					ASSERT(victimCPUEntry->GetTotalThreadCount() >=0);
					victimCPUEntry->MinVirtualRuntime();

					TRACE_SCHED_BL_STEAL("  SUCCESS: CPU %" B_PRId32 "(C%d,T%d) STOLE T%" B_PRId32 " (Lag %" B_PRId64 ") from CPU %" B_PRId32 "(C%d,T%d)\n",
						thiefCPU->ID(), thiefCPU->Core()->ID(), thiefCoreType,
						stolenTask->GetThread()->id, stolenTask->Lag(),
						victimCpuID, victimCPUEntry->Core()->ID(), victimCoreType);
				}
			}
		}
	}
	victimCPUEntry->UnlockRunQueue();

	if (stolenTask != NULL) {
		stolenTask->MarkDequeued();
		stolenTask->SetLastMigrationTime(system_time());
		if (stolenTask->Core() != NULL)
			stolenTask->UnassignCore(false);
	}
	return stolenTask;
}

static Scheduler::ThreadData*
scheduler_try_work_steal(Scheduler::CPUEntry* thiefCPU)
{
	SCHEDULER_ENTER_FUNCTION();
	Scheduler::ThreadData* stolenTask = NULL;
	int32 numCPUs = smp_get_num_cpus();
	int32 thiefCpuID = thiefCPU->ID();
	CoreEntry* thiefCore = thiefCPU->Core();
	if (thiefCore == NULL)
		return NULL;
	PackageEntry* thiefPackage = thiefCore->Package();
	if (thiefPackage == NULL)
		return NULL;

    if (thiefCore != NULL) {
        CPUSet sameCoreCPUs = thiefCore->CPUMask();
        for (int32 victimCpuID = 0; victimCpuID < numCPUs; victimCpuID++) {
            if (!sameCoreCPUs.GetBit(victimCpuID) || victimCpuID == thiefCpuID)
                continue;

            TRACE_SCHED_SMT_STEAL("WorkSteal: CPU %" B_PRId32 " (thief) considering SMT sibling CPU %" B_PRId32 " as victim.\n",
                thiefCpuID, victimCpuID);
            stolenTask = _attempt_one_steal(thiefCPU, victimCpuID);
            if (stolenTask != NULL) {
                TRACE_SCHED_SMT_STEAL("WorkSteal: CPU %" B_PRId32 " STOLE task %" B_PRId32 " from SMT sibling CPU %" B_PRId32 "\n",
                    thiefCpuID, stolenTask->GetThread()->id, victimCpuID);
                return stolenTask;
            }
        }
    }

    if (thiefPackage != NULL) {
        for (int32 coreIdx = 0; coreIdx < gCoreCount; coreIdx++) {
            CoreEntry* victimCore = &gCoreEntries[coreIdx];
            if (victimCore == thiefCore || victimCore->Package() != thiefPackage || victimCore->IsDefunct())
                continue;

            CPUSet victimCoreCPUs = victimCore->CPUMask();
            for (int32 victimCpuID = 0; victimCpuID < numCPUs; victimCpuID++) {
                 if (!victimCoreCPUs.GetBit(victimCpuID))
			continue;
                 stolenTask = _attempt_one_steal(thiefCPU, victimCpuID);
                 if (stolenTask != NULL) {
                     TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " stole from same package, diff core (CPU %" B_PRId32 " on Core %" B_PRId32 ")\n",
                         thiefCpuID, victimCpuID, victimCore->ID());
                     return stolenTask;
                 }
            }
        }
    }

    int32 startCpuIndex = get_random<int32>() % numCPUs;
    for (int32 i = 0; i < numCPUs; i++) {
        int32 victimCpuID = (startCpuIndex + i) % numCPUs;
        if (victimCpuID == thiefCpuID) continue;

        Scheduler::CPUEntry* victimCPUEntry = Scheduler::CPUEntry::GetCPU(victimCpuID);
        if (victimCPUEntry == NULL || victimCPUEntry->Core() == NULL) continue;

        if (thiefPackage != NULL && victimCPUEntry->Core()->Package() == thiefPackage) {
            continue;
        }

        stolenTask = _attempt_one_steal(thiefCPU, victimCpuID);
        if (stolenTask != NULL) {
            TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " stole from other package (CPU %" B_PRId32 ")\n", thiefCpuID, victimCpuID);
            return stolenTask;
        }
    }

	TRACE_SCHED("WorkSteal: Adv CPU %" B_PRId32 " found no task to steal after checking all levels.\n", thiefCpuID);
	return NULL;
}


static bool
update_old_thread_state(Thread* oldThread, int32 nextState, CoreEntry* core)
{
	Scheduler::ThreadData* oldThreadData = oldThread->scheduler_data;
	bool shouldReEnqueueOldThread = false;
	switch (nextState) {
		case B_THREAD_RUNNING:
		case B_THREAD_READY:
		{
			shouldReEnqueueOldThread = true;
			CPUSet oldThreadAffinity = oldThreadData->GetCPUMask();
			bool useAffinity = !oldThreadAffinity.IsEmpty();

			if (oldThreadData->IsIdle()
				|| (useAffinity
					&& !oldThreadAffinity.GetBit(smp_get_current_cpu()))) {
				shouldReEnqueueOldThread = false;
				if (!oldThreadData->IsIdle() && oldThreadData->Core() == core) {
					oldThreadData->UnassignCore(false);
				}
			} else {
				oldThreadData->Continues();
				oldThreadData->UpdateEevdfParameters(
					CPUEntry::GetCPU(smp_get_current_cpu()), false, true);
				TRACE_SCHED("reschedule: oldT %" B_PRId32 " re-q (after"
					" UpdateEevdfParameters), new VD %" B_PRId64 ", new Lag %"
					B_PRId64 "\n",
					oldThread->id, oldThreadData->VirtualDeadline(),
					oldThreadData->Lag());
			}
			break;
		}
		case THREAD_STATE_FREE_ON_RESCHED:
			oldThreadData->Dies();
			shouldReEnqueueOldThread = false;
			break;
		default:
			oldThreadData->GoesAway();
			shouldReEnqueueOldThread = false;
			break;
	}
	oldThread->has_yielded = false;
	return shouldReEnqueueOldThread;
}



static void
reschedule(int32 nextState)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	int32 thisCPUId = smp_get_current_cpu();
	gCPU[thisCPUId].invoke_scheduler = false;

	Scheduler::CPUEntry* cpu = Scheduler::CPUEntry::GetCPU(thisCPUId);
	CoreEntry* core = cpu->Core();

	Thread* oldThread = thread_get_current_thread();

	Scheduler::ThreadData* oldThreadData = oldThread->scheduler_data;
	if (oldThreadData != NULL)
		oldThreadData->StopCPUTime();

	TRACE_SCHED("reschedule (EEVDF): cpu %" B_PRId32 ", oldT %" B_PRId32 " (VD %" B_PRId64 ", Lag %" B_PRId64 ", VRun %" B_PRId64 ", Elig %" B_PRId64 ", state %s), next_state %" B_PRId32 "\n",
		thisCPUId, oldThread->id, oldThreadData->VirtualDeadline(), oldThreadData->Lag(), oldThreadData->VirtualRuntime(), oldThreadData->EligibleTime(),
		get_thread_state_name(oldThread->state), nextState);

	oldThread->state = nextState;
	oldThreadData->SetStolenInterruptTime(gCPU[thisCPUId].interrupt_time);

	bigtime_t actualRuntime = oldThreadData->TimeUsedInCurrentQuantum();

	if (!oldThreadData->IsIdle()) {
				if (nextState == B_THREAD_WAITING || nextState == B_THREAD_ASLEEP) {
			oldThreadData->RecordVoluntarySleepAndUpdateBurstTime(actualRuntime);
		}

		int32 weight = scheduler_priority_to_weight(oldThreadData->GetThread(), cpu);
		if (weight <= 0)
			weight = 1;

		uint32 coreCapacity = SCHEDULER_NOMINAL_CAPACITY;
		CoreEntry* runningCore = oldThreadData->Core();
		if (runningCore != NULL && runningCore->PerformanceCapacity() > 0) {
			coreCapacity = runningCore->PerformanceCapacity();
		} else if (runningCore != NULL && runningCore->PerformanceCapacity() == 0) {
			TRACE_SCHED_WARNING("reschedule: oldT %" B_PRId32 " on Core %" B_PRId32 " has 0 performance capacity! Using nominal %u.\n",
				oldThread->id, runningCore->ID(), SCHEDULER_NOMINAL_CAPACITY);
		} else if (runningCore == NULL) {
			TRACE_SCHED_WARNING("reschedule: oldT %" B_PRId32 " has NULL CoreEntry! Using nominal capacity %u for VR update.\n",
				oldThread->id, SCHEDULER_NOMINAL_CAPACITY);
		}

		uint64 numerator = (uint64)actualRuntime * coreCapacity * SCHEDULER_WEIGHT_SCALE;
		uint64 denominator = (uint64)SCHEDULER_NOMINAL_CAPACITY * weight;
		bigtime_t weightedRuntimeContribution;

		if (denominator == 0) {
			weightedRuntimeContribution = 0;
			TRACE_SCHED_WARNING("reschedule: oldT %" B_PRId32 " - denominator zero in VR update! actualRuntime %" B_PRId64 ", coreCap %" B_PRIu32 ", weight %" B_PRId32 "\n",
				oldThread->id, actualRuntime, coreCapacity, weight);
		} else {
			weightedRuntimeContribution = numerator / denominator;
		}

		oldThreadData->AddVirtualRuntime(weightedRuntimeContribution);
		TRACE_SCHED("reschedule: oldT %" B_PRId32 " ran %" B_PRId64 "us (wall), coreCap %" B_PRIu32 ", normWorkEqTime ~%" B_PRId64 "us, vruntime advanced by %" B_PRId64 " to %" B_PRId64 " (weight %" B_PRId32 ")\n",
			oldThread->id, actualRuntime, coreCapacity, ((uint64)actualRuntime * coreCapacity) / SCHEDULER_NOMINAL_CAPACITY,
			weightedRuntimeContribution, oldThreadData->VirtualRuntime(), weight);

		oldThreadData->AddLag(-weightedRuntimeContribution);
		TRACE_SCHED("reschedule: oldT %" B_PRId32 " lag reduced by %" B_PRId64 " (normalized weighted) to %" B_PRId64 "\n",
			oldThread->id, weightedRuntimeContribution, oldThreadData->Lag());
	}

	bool shouldReEnqueueOldThread
		= update_old_thread_state(oldThread, nextState, core);


	Scheduler::ThreadData* nextThreadData = NULL;
	cpu->LockRunQueue();

	if (gCPU[thisCPUId].disabled) {
		if (oldThread != NULL && !oldThreadData->IsIdle()) {
			TRACE_SCHED("reschedule: CPU %" B_PRId32 " disabling, re-homing T %" B_PRId32 "\n", thisCPUId, oldThread->id);

			if (oldThreadData->IsEnqueued() && oldThreadData->Core() == core) {
				cpu->GetEevdfScheduler().RemoveThread(oldThreadData);
				oldThreadData->MarkDequeued();
			}
            if (oldThreadData->Core() == core) {
                oldThreadData->UnassignCore(true);
            }

			cpu->UnlockRunQueue();

			atomic_set((int32*)&oldThread->state, B_THREAD_READY);
			scheduler_enqueue_in_run_queue(oldThread);

			cpu->LockRunQueue();
		}
		nextThreadData = cpu->PeekIdleThread();
		if (nextThreadData == NULL)
			panic("reschedule: No idle thread on disabling CPU %" B_PRId32 "!", thisCPUId);
	} else {
		Scheduler::ThreadData* oldThreadToConsider = (shouldReEnqueueOldThread && !oldThreadData->IsIdle())
			? oldThreadData : NULL;
		nextThreadData = cpu->ChooseNextThread(oldThreadToConsider, false, 0);

		if (nextThreadData->IsIdle() && !gSingleCore  ) {
			bool shouldAttemptSteal = (system_time() >= cpu->fNextStealAttemptTime);

		if (gCurrentMode != NULL && gCurrentMode->is_cpu_effectively_parked != NULL) {
			if (gCurrentMode->is_cpu_effectively_parked(cpu)) {
				shouldAttemptSteal = false;
				TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " is parked by current mode, skipping steal attempt.\n", cpu->ID());
			}
		}

			if (shouldAttemptSteal) {
				cpu->UnlockRunQueue();
				Scheduler::ThreadData* actuallyStolenThreadData = scheduler_try_work_steal(cpu);
				cpu->LockRunQueue();

				if (actuallyStolenThreadData != NULL) {
					InterruptsSpinLocker schedulerLocker(actuallyStolenThreadData->GetThread()->scheduler_lock);
					actuallyStolenThreadData->UpdateEevdfParameters(cpu, true, false);
					schedulerLocker.Unlock();

					TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " successfully STOLE T %" B_PRId32 " (after UpdateEevdfParameters). VD %" B_PRId64 ", Lag %" B_PRId64 "\n",
						cpu->ID(), actuallyStolenThreadData->GetThread()->id, actuallyStolenThreadData->VirtualDeadline(), actuallyStolenThreadData->Lag());

					nextThreadData = actuallyStolenThreadData;
					cpu->fNextStealAttemptTime = system_time() + kStealSuccessCooldownPeriod;

					if (actuallyStolenThreadData->Core() != cpu->Core()) {
						InterruptsSpinLocker lock(actuallyStolenThreadData->GetThread()->scheduler_lock);
						if (actuallyStolenThreadData->Core() != NULL)
							actuallyStolenThreadData->UnassignCore(false);
						actuallyStolenThreadData->MarkEnqueued(cpu->Core());
						lock.Unlock();
					} else if (!actuallyStolenThreadData->IsEnqueued()) {
						InterruptsSpinLocker lock(actuallyStolenThreadData->GetThread()->scheduler_lock);
						actuallyStolenThreadData->MarkEnqueued(cpu->Core());
						lock.Unlock();
					}
					int32 threadCount = cpu->GetTotalThreadCount();
					atomic_add(&threadCount, 1);
				} else {
					cpu->fNextStealAttemptTime = system_time() + kStealFailureBackoffInterval;
				}
			}
		}
	}

	if (!gCPU[thisCPUId].disabled)
		cpu->MinVirtualRuntime();
	cpu->UnlockRunQueue();

	Thread* nextThread = nextThreadData->GetThread();
	ASSERT(nextThread != NULL);
	ASSERT(!gCPU[thisCPUId].disabled || nextThreadData->IsIdle());

	if (nextThread != oldThread)
		acquire_spinlock(&nextThread->scheduler_lock);

	TRACE_SCHED("reschedule: cpu %" B_PRId32 " selected nextT %" B_PRId32 " (VD %" B_PRId64 ", Lag %" B_PRId64 ", Elig %" B_PRId64 ")\n",
		thisCPUId, nextThread->id, nextThreadData->VirtualDeadline(), nextThreadData->Lag(), nextThreadData->EligibleTime());

	T(ScheduleThread(nextThread, oldThread));
	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled, oldThread, nextThread);

	if (!nextThreadData->IsIdle()) {
		ASSERT(nextThreadData->Core() == core && "Scheduled non-idle EEVDF thread not on correct core!");
	} else {
		ASSERT(nextThreadData->Core() == core && "Idle EEVDF thread not on correct core!");
	}

	nextThread->state = B_THREAD_RUNNING;
	nextThreadData->StartCPUTime();
	cpu->TrackActivity(oldThreadData, nextThreadData);

	bigtime_t sliceForTimer = 0;
	if (!nextThreadData->IsIdle()) {
		sliceForTimer = nextThreadData->SliceDuration();
		nextThreadData->StartQuantum(sliceForTimer);
		TRACE_SCHED("reschedule: nextT %" B_PRId32 " starting EEVDF slice %" B_PRId64 " on CPU %" B_PRId32 "\n",
			nextThread->id, sliceForTimer, thisCPUId);
	} else {
		sliceForTimer = kLoadMeasureInterval * 2;
		nextThreadData->StartQuantum(B_INFINITE_TIMEOUT);
	}

	cpu->StartQuantumTimer(nextThreadData, gCPU[thisCPUId].preempted, sliceForTimer);
	gCPU[thisCPUId].preempted = false;

	if (!nextThreadData->IsIdle()) {
		nextThreadData->Continues();
	} else if (gCurrentMode != NULL) {
		gCurrentMode->rebalance_irqs(true /* CPU is now idle */);
	}

	SCHEDULER_EXIT_FUNCTION();

	if (nextThread != oldThread) {
		thread_resumes(nextThread);
	}
}

void
scheduler_reschedule()
{
	reschedule(B_THREAD_READY);
}


// --- Mechanism A: Task-Contextual IRQ Re-evaluation Helper ---
// Define constants for Mechanism A
#define IRQ_INTERFERENCE_LOAD_THRESHOLD (kMaxLoad / 20) // e.g., 50 for kMaxLoad = 1000
#define DYNAMIC_IRQ_MOVE_COOLDOWN (150000)          // 150ms


status_t
scheduler_on_thread_create(Thread* thread, bool idleThread)
{
	thread->scheduler_data = new(std::nothrow) Scheduler::ThreadData(thread);
	if (thread->scheduler_data == NULL) return B_NO_MEMORY;
	return B_OK;
}


void
scheduler_on_thread_init(Thread* thread)
{
	ASSERT(thread->scheduler_data != NULL);
	Scheduler::ThreadData* threadData = thread->scheduler_data;

	if (thread_is_idle_thread(thread)) {
		static int32 sIdleThreadsCPUIDCounter = 0;
		int32 cpuID = sIdleThreadsCPUIDCounter++;

		if (cpuID < 0 || cpuID >= smp_get_num_cpus()) {
			panic("scheduler_on_thread_init: Invalid cpuID %" B_PRId32
				" for idle thread %" B_PRId32, cpuID, thread->id);
		}

		thread->previous_cpu = &gCPU[cpuID];
		thread->pinned_to_cpu = 1; // Pin idle threads to their CPU

		threadData->Init(CoreEntry::GetCore(cpuID));
		threadData->SetSliceDuration(B_INFINITE_TIMEOUT);
		threadData->SetVirtualDeadline(B_INFINITE_TIMEOUT);
		threadData->SetLag(0);
		threadData->SetEligibleTime(0);
		threadData->SetVirtualRuntime(0);

		Scheduler::CPUEntry::GetCPU(cpuID)->SetIdleThread(threadData);
		TRACE_SCHED("scheduler_on_thread_init (EEVDF): Initialized idle thread %" B_PRId32 " for CPU %" B_PRId32 "\n", thread->id, cpuID);

	} else {
		threadData->Init();
	}
}


void
scheduler_on_thread_destroy(Thread* thread)
{
    if (!thread || !thread->scheduler_data) return;

    Scheduler::ThreadData* data = thread->scheduler_data;
    int32 irqs[4];
    int8 count = 0;

InterruptsSpinLocker lock(thread->scheduler_lock);
    const int32* affIrqs = data->GetAffinitizedIrqs(count);
    if (count > 0) memcpy(irqs, affIrqs, count * sizeof(int32));
    data->ClearAffinitizedIrqs();
    lock.Unlock();

    for (int8 i = 0; i < count; ++i) {
        int32 irq = irqs[i];
        if (sIrqTaskAffinityMap) {
            InterruptsSpinLocker mapLock(gIrqTaskAffinityLock);
            thread_id* val = sIrqTaskAffinityMap->Lookup(irq);
            if (val && *val == thread->id) {
                sIrqTaskAffinityMap->Remove(val);
            }
        }
    }

    delete data;
}


void
scheduler_start()
{
	dprintf("scheduler_start: entry\n");
	InterruptsSpinLocker _(thread_get_current_thread()->scheduler_lock);
	SCHEDULER_ENTER_FUNCTION();
	reschedule(B_THREAD_READY);
}


status_t
scheduler_set_operation_mode(scheduler_mode mode)
{
	if (mode != SCHEDULER_MODE_LOW_LATENCY && mode != SCHEDULER_MODE_POWER_SAVING)
		return B_BAD_VALUE;

	InterruptsBigSchedulerLocker lock;

	if (gCurrentModeID == mode && gCurrentMode != NULL) {
		dprintf("scheduler: Mode %d (%s) already set.\n", mode, gCurrentMode->name);
	}

	dprintf("scheduler: switching to %s mode\n", sSchedulerModes[mode]->name);

	gCurrentModeID = mode;
	gCurrentMode = sSchedulerModes[mode];

	// gKernelKDistFactor = DEFAULT_K_DIST_FACTOR; // REMOVED
	gSchedulerLoadBalancePolicy = Scheduler::SPREAD;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;

	if (gCurrentMode->switch_to_mode != NULL) {
		gCurrentMode->switch_to_mode();
	} else {
		if (mode == SCHEDULER_MODE_POWER_SAVING) {
			// gKernelKDistFactor = 0.6f; // REMOVED
			gSchedulerLoadBalancePolicy = Scheduler::CONSOLIDATE;
			gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;
		}
	}

	cpu_set_scheduler_mode(mode);
	return B_OK;
}


void
scheduler_set_cpu_enabled(int32 cpuID, bool enabled)
{
#if KDEBUG
	if (are_interrupts_enabled())
		panic("scheduler_set_cpu_enabled: called with interrupts enabled");
#endif
	dprintf("scheduler: %s CPU %" B_PRId32 "\n", enabled ? "enabling" : "disabling", cpuID);
	InterruptsBigSchedulerLocker _;
	if (gCurrentMode != NULL && gCurrentMode->set_cpu_enabled != NULL) {
		gCurrentMode->set_cpu_enabled(cpuID, enabled);
	}
	Scheduler::CPUEntry* cpuEntry = Scheduler::CPUEntry::GetCPU(cpuID);
	CoreEntry* core = cpuEntry->Core();
	ASSERT(core->CPUCount() >= 0);

	if (enabled) {
		cpuEntry->Start();
	} else {
		TRACE_SCHED("scheduler_set_cpu_enabled: Disabling CPU %" B_PRId32 ". Migrating its queued threads.\n", cpuID);

		cpuEntry->LockRunQueue();
		EevdfScheduler& runQueue = cpuEntry->GetEevdfScheduler();
		DoublyLinkedList<Scheduler::ThreadData> threadsToReenqueue;

		while (true) {
			struct thread* thread = runQueue.PopMinThread();
			if (thread == NULL)
				break;
			((Scheduler::ThreadData*)thread->scheduler_data)->MarkDequeued();
			if (((Scheduler::ThreadData*)thread->scheduler_data)->Core() == core) {
				((Scheduler::ThreadData*)thread->scheduler_data)->UnassignCore(false);
			}
			threadsToReenqueue.Add((Scheduler::ThreadData*)thread->scheduler_data);
		}
		cpuEntry->UnlockRunQueue();

		Scheduler::ThreadData* threadToReenqueue;
		while ((threadToReenqueue = threadsToReenqueue.RemoveHead()) != NULL) {
			TRACE_SCHED("scheduler_set_cpu_enabled: Re-homing T %" B_PRId32 " from disabled CPU %" B_PRId32 "\n",
				threadToReenqueue->GetThread()->id, cpuID);
			atomic_set((int32*)&threadToReenqueue->GetThread()->state, B_THREAD_READY);
			scheduler_enqueue_in_run_queue(threadToReenqueue->GetThread());
		}
		ThreadEnqueuer enqueuer;
		core->RemoveCPU(cpuEntry, enqueuer);
	}

	gCPU[cpuID].disabled = !enabled;
	if (enabled)
		Scheduler::gCPUEnabled.SetBitAtomic(cpuID);
	else
		Scheduler::gCPUEnabled.ClearBitAtomic(cpuID);

	if (!enabled) {
		cpuEntry->Stop();
		if (smp_get_current_cpu() != cpuID)
			smp_send_ici(cpuID, SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
	}
}


static void
traverse_topology_tree(const cpu_topology_node* node, int packageID, int coreID)
{
	switch (node->level) {
		case CPU_TOPOLOGY_SMT:
			sCPUToCore[node->id] = coreID; sCPUToPackage[node->id] = packageID; return;
		case CPU_TOPOLOGY_CORE: coreID = node->id; break;
		case CPU_TOPOLOGY_PACKAGE: packageID = node->id; break;
		default: break;
	}
	for (int32 i = 0; i < node->children_count; i++)
		traverse_topology_tree(node->children[i], packageID, coreID);
}


static status_t
build_topology_mappings(int32& cpuCount, int32& coreCount, int32& packageCount)
{
	cpuCount = smp_get_num_cpus();
	sCPUToCore = new(std::nothrow) int32[cpuCount];
	if (sCPUToCore == NULL) return B_NO_MEMORY;
	ArrayDeleter<int32> cpuToCoreDeleter(sCPUToCore);
	sCPUToPackage = new(std::nothrow) int32[cpuCount];
	if (sCPUToPackage == NULL) return B_NO_MEMORY;
	ArrayDeleter<int32> cpuToPackageDeleter(sCPUToPackage);
	coreCount = 0;
	for (int32 i = 0; i < cpuCount; i++) {
		if (gCPU[i].topology_id[CPU_TOPOLOGY_SMT] == 0) coreCount++;
	}
	packageCount = 0;
	for (int32 i = 0; i < cpuCount; i++) {
		if (gCPU[i].topology_id[CPU_TOPOLOGY_SMT] == 0 && gCPU[i].topology_id[CPU_TOPOLOGY_CORE] == 0)
			packageCount++;
	}
	const cpu_topology_node* root = get_cpu_topology();
	traverse_topology_tree(root, 0, 0);
	cpuToCoreDeleter.Detach();
	cpuToPackageDeleter.Detach();
	return B_OK;
}


static status_t
init()
{
	int32 cpuCount, coreCount, packageCount;
	status_t result = build_topology_mappings(cpuCount, coreCount, packageCount);
	if (result != B_OK) return result;
	gSingleCore = coreCount == 1;
	scheduler_update_policy();
	gCoreCount = coreCount;
	gPackageCount = packageCount;
	gCPUEntries = new(std::nothrow) Scheduler::CPUEntry[cpuCount]();
	if (gCPUEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<Scheduler::CPUEntry> cpuEntriesDeleter(gCPUEntries);
	gCoreEntries = new(std::nothrow) CoreEntry[coreCount]();
	if (gCoreEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<CoreEntry> coreEntriesDeleter(gCoreEntries);
	gPackageEntries = new(std::nothrow) PackageEntry[packageCount]();
	if (gPackageEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<PackageEntry> packageEntriesDeleter(gPackageEntries);

	for (int32 i = 0; i < Scheduler::kNumCoreLoadHeapShards; i++) {
		int32 shardHeapSize = gCoreCount / Scheduler::kNumCoreLoadHeapShards + 4;
		new(&Scheduler::gCoreLoadHeapShards[i]) CoreLoadHeap(shardHeapSize);
		new(&Scheduler::gCoreHighLoadHeapShards[i]) CoreLoadHeap(shardHeapSize);
		B_INITIALIZE_RW_SPINLOCK(&gCoreHeapsShardLock[i]);
		dprintf("gCoreHeapsShardLock[%" B_PRId32 "] at %p\n", i, &gCoreHeapsShardLock[i]);
	}
	new(&gIdlePackageList) IdlePackageList;
	dprintf("gIdlePackageLock at %p\n", &gIdlePackageLock);

	for (int32 i = 0; i < SMP_MAX_CPUS; i++) {
		atomic_set64(&gReportedCpuMinVR[i], 0);
	}

	for (int32 i = 0; i < packageCount; ++i) {
		gPackageEntries[i].Init(i);
	}

	for (int32 i = 0; i < packageCount; i++)
		gPackageEntries[i].Init(i);

	for (int32 i = 0; i < coreCount; i++) {
		int32 packageIdx = -1;
		for (int32 j = 0; j < cpuCount; j++) {
			if (sCPUToCore[j] == i) {
				packageIdx = sCPUToPackage[j];
				break;
			}
		}
		ASSERT(packageIdx >= 0 && packageIdx < packageCount);
		gCoreEntries[i].Init(i, &gPackageEntries[packageIdx]);
	}

	for (int32 i = 0; i < cpuCount; i++) {
		int32 coreIdx = sCPUToCore[i];
		gCPUEntries[i].Init(i, &gCoreEntries[coreIdx]);
		gCoreEntries[coreIdx].AddCPU(&gCPUEntries[i]);
	}

	packageEntriesDeleter.Detach();
	coreEntriesDeleter.Detach();
	cpuEntriesDeleter.Detach();
	return B_OK;
}


namespace Scheduler {
// Global minimum virtual runtime for the system
bigtime_t gGlobalMinVirtualRuntime = 0;
spinlock gGlobalMinVRuntimeLock = B_SPINLOCK_INITIALIZER;
int64 gReportedCpuMinVR[SMP_MAX_CPUS];
}


static void
scheduler_update_global_min_vruntime()
{
	if (smp_get_num_cpus() == 1)
		return;

	bigtime_t calculatedNewGlobalMin = -1LL;

	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (!Scheduler::gCPUEnabled.GetBit(i))
			continue;
		bigtime_t cpuReportedMin = atomic_get64(&Scheduler::gReportedCpuMinVR[i]);
		if (calculatedNewGlobalMin == -1LL || cpuReportedMin < calculatedNewGlobalMin) {
			calculatedNewGlobalMin = cpuReportedMin;
		}
	}

	if (calculatedNewGlobalMin != -1LL) {
		InterruptsSpinLocker locker(gGlobalMinVRuntimeLock);
		bigtime_t currentGlobalVal = atomic_get64((int64*)&gGlobalMinVirtualRuntime);
		if (calculatedNewGlobalMin > currentGlobalVal) {
			atomic_set64((int64*)&gGlobalMinVirtualRuntime, calculatedNewGlobalMin);
			TRACE_SCHED("GlobalMinVRuntime updated to %" B_PRId64 "\n", calculatedNewGlobalMin);
		}
	}
}


static int32 scheduler_load_balance_event(timer* /*unused*/)
{
	if (!gSingleCore) {
		scheduler_update_global_min_vruntime();

		bool migrationOccurred = scheduler_perform_load_balance();

		if (migrationOccurred) {
			gDynamicLoadBalanceInterval = (bigtime_t)(gDynamicLoadBalanceInterval * kLoadBalanceIntervalDecreaseFactor);
			if (gDynamicLoadBalanceInterval < kMinLoadBalanceInterval)
				gDynamicLoadBalanceInterval = kMinLoadBalanceInterval;
			TRACE_SCHED("LoadBalanceEvent: Migration occurred. New interval: %" B_PRId64 "us\n", gDynamicLoadBalanceInterval);
		} else {
			gDynamicLoadBalanceInterval = (bigtime_t)(gDynamicLoadBalanceInterval * kLoadBalanceIntervalIncreaseFactor);
			if (gDynamicLoadBalanceInterval > kMaxLoadBalanceInterval)
				gDynamicLoadBalanceInterval = kMaxLoadBalanceInterval;
			TRACE_SCHED("LoadBalanceEvent: No migration. New interval: %" B_PRId64 "us\n", gDynamicLoadBalanceInterval);
		}
	}
    add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, gDynamicLoadBalanceInterval, B_ONE_SHOT_RELATIVE_TIMER);
    return B_HANDLED_INTERRUPT;
}


// static int cmd_scheduler_set_kdf(int argc, char** argv); // REMOVED
// static int cmd_scheduler_get_kdf(int argc, char** argv); // REMOVED
static int cmd_scheduler_set_smt_factor(int argc, char** argv);
static int cmd_scheduler_get_smt_factor(int argc, char** argv);


static void
_scheduler_init_kdf_debug_commands()
{
	add_debugger_command_etc("scheduler_set_smt_factor", &cmd_scheduler_set_smt_factor, "Set ... SMT conflict factor.", "<factor>\n...", 0);
	add_debugger_command_alias("set_smt_factor", "scheduler_set_smt_factor", "Alias for scheduler_set_smt_factor");
	add_debugger_command_etc("scheduler_get_smt_factor", &cmd_scheduler_get_smt_factor, "Get ... SMT conflict factor.", "...", 0);
	add_debugger_command_alias("get_smt_factor", "scheduler_get_smt_factor", "Alias for scheduler_get_smt_factor");
	// Removido: dump_eevdf_weights
}







void
scheduler_init()
{
	if (sSchedulerEnabled)
		return;

	int32 cpuCount = smp_get_num_cpus();
	dprintf("scheduler_init: found %" B_PRId32 " logical cpu%s and %" B_PRId32
		" cache level%s\n", cpuCount, cpuCount != 1 ? "s" : "",
		gCPUCacheLevelCount, gCPUCacheLevelCount != 1 ? "s" : "");
#ifdef SCHEDULER_PROFILING
	Profiling::Profiler::Initialize();
#endif
	status_t result = init();
	if (result != B_OK)
		panic("scheduler_init: failed to initialize scheduler\n");

	gDynamicLoadBalanceInterval = kInitialLoadBalanceInterval;

	scheduler_set_operation_mode(SCHEDULER_MODE_LOW_LATENCY);
	if (!gSingleCore) {
		add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, gDynamicLoadBalanceInterval, B_ONE_SHOT_RELATIVE_TIMER);
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	}
	Scheduler::init_debug_commands();
	_scheduler_init_kdf_debug_commands();
	add_debugger_command_etc("thread_sched_info", &cmd_thread_sched_info, "Dump detailed scheduler information for a specific thread", "<thread_id>\n...", 0);

	sIrqTaskAffinityMap = new(std::nothrow) BOpenHashTable<IntHashDefinition>;
	if (sIrqTaskAffinityMap != NULL)
		sIrqTaskAffinityMap->Init();
	for (int i = 0; i < NUM_IO_VECTORS; ++i) {
		atomic_set64(&gIrqLastFollowMoveTime[i], 0);
	}

	scheduler_init_weights();
	dprintf("scheduler_init: done\n");
}



// static const double KDF_DEBUG_MIN_FACTOR = 0.0; // REMOVED
// static const double KDF_DEBUG_MAX_FACTOR = 2.0; // REMOVED
static const double SMT_DEBUG_MIN_FACTOR = 0.0;
static const double SMT_DEBUG_MAX_FACTOR = 1.0;


static int
cmd_scheduler_set_smt_factor(int argc, char** argv)
{
	if (argc != 2) { kprintf("Usage: scheduler_set_smt_factor <factor (float)>\n"); return B_KDEBUG_ERROR; }
	char* endPtr;
	double newFactor = strtod(argv[1], &endPtr);
	if (argv[1] == endPtr || *endPtr != '\0') { kprintf("Error: Invalid float value for SMT factor: %s\n", argv[1]); return B_KDEBUG_ERROR; }
	if (newFactor < SMT_DEBUG_MIN_FACTOR || newFactor > SMT_DEBUG_MAX_FACTOR) { kprintf("Error: SMT factor %f is out of reasonable range [%.1f - %.1f]. Value not changed.\n", newFactor, SMT_DEBUG_MIN_FACTOR, SMT_DEBUG_MAX_FACTOR); return B_KDEBUG_ERROR; }
	Scheduler::gSchedulerSMTConflictFactor = (float)newFactor;
	kprintf("Scheduler gSchedulerSMTConflictFactor set to: %f\n", Scheduler::gSchedulerSMTConflictFactor);
	return 0;
}

static int
cmd_scheduler_get_smt_factor(int argc, char** argv)
{
	if (argc != 1) { kprintf("Usage: scheduler_get_smt_factor\n"); return B_KDEBUG_ERROR; }
	kprintf("Current scheduler gSchedulerSMTConflictFactor: %f\n", Scheduler::gSchedulerSMTConflictFactor);
	return 0;
}




static Scheduler::CPUEntry*
_scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqVector, int32 irqToMoveLoad)
{
	return SelectTargetCPUForIRQ(core, irqVector, irqToMoveLoad, gModeIrqTargetFactor,
		gSchedulerSMTConflictFactor, gModeMaxTargetCpuIrqLoad);
}

static int32
scheduler_irq_balance_event(timer* /* unused */)
{
	if (gSingleCore || !sSchedulerEnabled) {
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
		return B_HANDLED_INTERRUPT;
	}
	SCHEDULER_ENTER_FUNCTION();
	TRACE_SCHED_IRQ("Proactive IRQ Balance Check running\n");
	Scheduler::CPUEntry* sourceCpuMaxIrq = NULL;
	Scheduler::CPUEntry* targetCandidateCpuMinIrq = NULL;
	int32 maxIrqLoadFound = -1;
	int32 minIrqLoadFound = 0x7fffffff;
	int32 enabledCpuCount = 0;

	CoreEntry* preferredTargetCoreForPS = NULL;
	if (gCurrentModeID == SCHEDULER_MODE_POWER_SAVING && Scheduler::sSmallTaskCore != NULL) {
		CoreEntry* stc = Scheduler::sSmallTaskCore;
		bool stcHasEnabledCpu = false;
		if (!stc->IsDefunct()) {
			CPUSet stcCPUs = stc->CPUMask();
			for (int32 i = 0; i < smp_get_num_cpus(); ++i) {
				if (stcCPUs.GetBit(i) && Scheduler::gCPUEnabled.GetBit(i)) {
					stcHasEnabledCpu = true;
					break;
				}
			}
		}
		if (stcHasEnabledCpu) {
			preferredTargetCoreForPS = stc;
			TRACE_SCHED_IRQ("IRQBalance(PS): Preferred target core for IRQ consolidation is STC %" B_PRId32 " (Type %d)\n",
				stc->ID(), stc->Type());
		}
	}

	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (!Scheduler::gCPUEnabled.GetBit(i))
			continue;
		enabledCpuCount++;
		Scheduler::CPUEntry* currentCpu = Scheduler::CPUEntry::GetCPU(i);
		int32 currentTotalIrqLoad = currentCpu->CalculateTotalIrqLoad();

		if (sourceCpuMaxIrq == NULL || currentTotalIrqLoad > maxIrqLoadFound) {
			maxIrqLoadFound = currentTotalIrqLoad;
			sourceCpuMaxIrq = currentCpu;
		}

		bool isPreferredTarget = (preferredTargetCoreForPS != NULL && currentCpu->Core() == preferredTargetCoreForPS);
		int32 effectiveLoadForComparison = currentTotalIrqLoad;
		if (isPreferredTarget) {
			effectiveLoadForComparison -= kMaxLoad / 4;
			if (effectiveLoadForComparison < 0) effectiveLoadForComparison = 0;
		} else if (gCurrentModeID == SCHEDULER_MODE_POWER_SAVING && preferredTargetCoreForPS != NULL && currentCpu->Core()->Type() != CORE_TYPE_LITTLE) {
			effectiveLoadForComparison += kMaxLoad / 4;
		}

		if (targetCandidateCpuMinIrq == NULL || effectiveLoadForComparison < minIrqLoadFound) {
			if (currentCpu != sourceCpuMaxIrq || enabledCpuCount == 1) {
				minIrqLoadFound = effectiveLoadForComparison;
				targetCandidateCpuMinIrq = currentCpu;
			}
		}
	}

	if (targetCandidateCpuMinIrq == NULL || (targetCandidateCpuMinIrq == sourceCpuMaxIrq && enabledCpuCount > 1)) {
		minIrqLoadFound = 0x7fffffff;
		Scheduler::CPUEntry* generalFallbackTarget = NULL;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (!Scheduler::gCPUEnabled.GetBit(i) || Scheduler::CPUEntry::GetCPU(i) == sourceCpuMaxIrq)
				continue;
			Scheduler::CPUEntry* potentialTarget = Scheduler::CPUEntry::GetCPU(i);
			int32 potentialTargetLoad = potentialTarget->CalculateTotalIrqLoad();
			if (generalFallbackTarget == NULL || potentialTargetLoad < minIrqLoadFound) {
				generalFallbackTarget = potentialTarget;
				minIrqLoadFound = potentialTargetLoad;
			}
		}
		targetCandidateCpuMinIrq = generalFallbackTarget;
	}

	if (sourceCpuMaxIrq == NULL || targetCandidateCpuMinIrq == NULL || sourceCpuMaxIrq == targetCandidateCpuMinIrq) {
		TRACE_SCHED_IRQ("Proactive IRQ: No suitable distinct source/target pair or no CPUs enabled.\n");
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
		return B_HANDLED_INTERRUPT;
	}

	int32 actualTargetMinIrqLoad = targetCandidateCpuMinIrq->CalculateTotalIrqLoad();
	if (maxIrqLoadFound > gHighAbsoluteIrqThreshold && maxIrqLoadFound > actualTargetMinIrqLoad + gSignificantIrqLoadDifference) {
		TRACE_SCHED_IRQ("Proactive IRQ: Imbalance detected. Source CPU %" B_PRId32 " (IRQ load %" B_PRId32 ") vs Target Cand. CPU %" B_PRId32 " (Actual IRQ load %" B_PRId32 ")\n",
			sourceCpuMaxIrq->ID(), maxIrqLoadFound, targetCandidateCpuMinIrq->ID(), actualTargetMinIrqLoad);
		irq_assignment* candidateIRQs[DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY];
		int32 candidateCount = 0;
		{
			cpu_ent* cpuSt = &gCPU[sourceCpuMaxIrq->ID()];
			SpinLocker locker(cpuSt->irqs_lock);
			irq_assignment* irq = (irq_assignment*)list_get_first_item(&cpuSt->irqs);
			while (irq != NULL) {
				if (candidateCount < gMaxIRQsToMoveProactively) {
					candidateIRQs[candidateCount++] = irq;
					for (int k = candidateCount - 1; k > 0; --k) {
						if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
							std::swap(candidateIRQs[k], candidateIRQs[k-1]);
						} else break;
					}
				} else if (gMaxIRQsToMoveProactively > 0 && irq->load > candidateIRQs[gMaxIRQsToMoveProactively - 1]->load) {
					candidateIRQs[gMaxIRQsToMoveProactively - 1] = irq;
					for (int k = gMaxIRQsToMoveProactively - 1; k > 0; --k) {
						if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
							std::swap(candidateIRQs[k], candidateIRQs[k-1]);
						} else break;
					}
				}
				irq = (irq_assignment*)list_get_next_item(&cpuSt->irqs, irq);
			}
		}
		for (int32 i = 0; i < candidateCount; i++) {
			irq_assignment* irqToMove = candidateIRQs[i];
			if (irqToMove == NULL) continue;

			CoreEntry* preferredTargetCore = targetCandidateCpuMinIrq->Core();
			bool hasAffinity = false;

			Scheduler::CPUEntry* finalTargetCpu = _scheduler_select_cpu_for_irq(preferredTargetCore, irqToMove->irq, irqToMove->load);

			if (finalTargetCpu != NULL && finalTargetCpu != sourceCpuMaxIrq) {
				bigtime_t now = system_time();
				bigtime_t cooldownToRespect = kIrqFollowTaskCooldownPeriod;
				bool proceedWithMove = false;
				bigtime_t lastRecordedMoveTime = atomic_get64(&gIrqLastFollowMoveTime[irqToMove->irq]);

				if (now >= lastRecordedMoveTime + cooldownToRespect) {
					if (atomic_test_and_set64(&gIrqLastFollowMoveTime[irqToMove->irq], now, lastRecordedMoveTime)) {
						proceedWithMove = true;
					} else {
						TRACE_SCHED_IRQ("Periodic IRQ Balance: CAS failed for IRQ %d, move deferred due to concurrent update.\n", irqToMove->irq);
					}
				} else {
					TRACE_SCHED_IRQ("Periodic IRQ Balance: IRQ %d for T %" B_PRId32 " is in cooldown (last move at %" B_PRId64 ", now %" B_PRId64 ", cooldown %" B_PRId64 "). Skipping move.\n",
						irqToMove->irq, -1, lastRecordedMoveTime, now, cooldownToRespect);
				}

				if (proceedWithMove) {
					TRACE_SCHED_IRQ("Periodic IRQ Balance: Moving IRQ %d (load %" B_PRId32 ") from CPU %" B_PRId32 " (core %" B_PRId32 ") to CPU %" B_PRId32 " (core %" B_PRId32 ")%s\n",
						irqToMove->irq, irqToMove->load,
						sourceCpuMaxIrq->ID(), sourceCpuMaxIrq->Core()->ID(),
						finalTargetCpu->ID(), finalTargetCpu->Core()->ID(),
						hasAffinity ? " (affinity considered)" : "");
					if (hasAffinity) {
						// do nothing
					}
					assign_io_interrupt_to_cpu(irqToMove->irq, finalTargetCpu->ID());
				}
			} else {
				TRACE_SCHED_IRQ("Periodic IRQ Balance: No suitable target CPU found for IRQ %d on core %" B_PRId32 " or target is source. IRQ remains on CPU %" B_PRId32 ".\n",
					irqToMove->irq, preferredTargetCore->ID(), sourceCpuMaxIrq->ID());
			}
		}
	} else {
		TRACE("Proactive IRQ: No significant imbalance meeting thresholds (maxLoad: %" B_PRId32 ", minLoad: %" B_PRId32 ").\n", maxLoadFound, minIrqLoadFound);
	}
	add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	return B_HANDLED_INTERRUPT;
}


void
scheduler_enable_scheduling()
{
	sSchedulerEnabled = true;
}


void
scheduler_update_policy()
{
	gTrackCPULoad = increase_cpu_performance(0) == B_OK;
	gTrackCoreLoad = !gSingleCore || gTrackCPULoad;
	dprintf("scheduler switches: single core: %s, cpu load tracking: %s,"
		" core load tracking: %s\n", gSingleCore ? "true" : "false",
		gTrackCPULoad ? "true" : "false",
		gTrackCoreLoad ? "true" : "false");
}


SchedulerListener::~SchedulerListener()
{
}


extern "C" void scheduler_dump_cpu_heap(int32 cpu)
{
	if (cpu < 0 || cpu >= smp_get_num_cpus()) {
		kprintf("Invalid CPU number %" B_PRId32 "\n", cpu);
		return;
	}
	Scheduler::CPUEntry* cpuEntry = Scheduler::CPUEntry::GetCPU(cpu);
	if (cpuEntry == NULL || cpuEntry->Core() == NULL) {
		kprintf("CPU %" B_PRId32 " not properly initialized in scheduler.\n", cpu);
		return;
	}
	cpuEntry->Core()->CPUHeap()->Dump();
}


void
scheduler_add_listener(struct SchedulerListener* listener)
{
	InterruptsSpinLocker _(sSchedulerListenersLock);
	gSchedulerListeners.Add(listener);
}


void
scheduler_remove_listener(struct SchedulerListener* listener)
{
	InterruptsSpinLocker _(sSchedulerListenersLock);
	gSchedulerListeners.Remove(listener);
}

static Scheduler::CPUEntry*
_scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest,
	const Scheduler::ThreadData* affinityCheckThread)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	Scheduler::CPUEntry* bestCPU = NULL;
	int32 bestScore = preferBusiest ? 0x7fffffff : -1;

	core->LockCPUHeap();

	CPUSet coreCPUs = core->CPUMask();
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (!coreCPUs.GetBit(i) || gCPU[i].disabled)
			continue;

		Scheduler::CPUEntry* currentCPU = Scheduler::CPUEntry::GetCPU(i);
		ASSERT(currentCPU->Core() == core);

		if (affinityCheckThread != NULL) {
			const CPUSet& threadAffinity = affinityCheckThread->GetCPUMask();
			if (!threadAffinity.IsEmpty() && !threadAffinity.GetBit(i))
				continue;
		}

		float effectiveSmtLoad;
		int32 currentSmtScore = currentCPU->_CalculateSmtAwareKey(effectiveSmtLoad);

		bool isBetter = false;
		if (bestCPU == NULL) {
			isBetter = true;
		} else {
			if (preferBusiest) {
				if (currentSmtScore < bestScore) {
					isBetter = true;
				} else if (currentSmtScore == bestScore) {
					if (currentCPU->ID() > bestCPU->ID())
						isBetter = true;
				}
			} else {
				if (currentSmtScore > bestScore) {
					isBetter = true;
				} else if (currentSmtScore == bestScore) {
					int32 currentQueueDepth = currentCPU->GetEevdfScheduler().Count();
					int32 bestQueueDepth = bestCPU->GetEevdfScheduler().Count();
					if (currentQueueDepth < bestQueueDepth) {
						isBetter = true;
						TRACE_SCHED_SMT_TIEBREAK("_select_cpu_on_core: CPU %" B_PRId32 " (score %" B_PRId32 ") ties with current best CPU %" B_PRId32 ". CPU %" B_PRId32 " selected due to shallower run queue (%d vs %d).\n",
							currentCPU->ID(), currentSmtScore, bestCPU->ID(), currentCPU->ID(), currentQueueDepth, bestQueueDepth);
					} else if (currentQueueDepth == bestQueueDepth) {
						bigtime_t currentMinVR = currentCPU->GetCachedMinVirtualRuntime();
						bigtime_t bestMinVR = bestCPU->GetCachedMinVirtualRuntime();
						if (currentMinVR < bestMinVR) {
							isBetter = true;
							TRACE_SCHED_SMT_TIEBREAK("_select_cpu_on_core: CPU %" B_PRId32 " (score %" B_PRId32 ") ties with current best CPU %" B_PRId32 " (queue depth %d). CPU %" B_PRId32 " selected due to lower MinVirtualRuntime (%" B_PRId64 " vs %" B_PRId64 ").\n",
								currentCPU->ID(), currentSmtScore, bestCPU->ID(), currentQueueDepth, currentCPU->ID(), currentMinVR, bestMinVR);
						} else if (currentMinVR == bestMinVR) {
							if (currentCPU->ID() < bestCPU->ID()) {
								isBetter = true;
								TRACE_SCHED_SMT_TIEBREAK("_select_cpu_on_core: CPU %" B_PRId32 " (score %" B_PRId32 ") ties with current best CPU %" B_PRId32 " (queue %d, MinVR %" B_PRId64 "). CPU %" B_PRId32 " selected due to lower CPU ID (%d vs %d).\n",
									currentCPU->ID(), currentSmtScore, bestCPU->ID(), currentQueueDepth, currentMinVR, currentCPU->ID(), currentCPU->ID(), bestCPU->ID());
							}
						}
					}
				}
			}
		}

		if (isBetter) {
			bestScore = currentSmtScore;
			bestCPU = currentCPU;
		}
	}
	core->UnlockCPUHeap();
	return bestCPU;
}


static const int32 kWorkDifferenceThresholdAbsolute = 200;
#define BL_TYPE_BONUS_PPREF_LITTLE_TO_BIG_LL (SCHEDULER_TARGET_LATENCY * 4)
#define BL_TYPE_PENALTY_PPREF_BIG_TO_LITTLE_LL (SCHEDULER_TARGET_LATENCY * 10)
#define BL_TYPE_BONUS_EPREF_BIG_TO_LITTLE_PS (SCHEDULER_TARGET_LATENCY * 2)
#define BL_TYPE_BONUS_PPREF_LITTLE_TO_BIG_PS (SCHEDULER_TARGET_LATENCY * 1)
#define BL_TYPE_PENALTY_EPREF_LITTLE_TO_BIG_PS (SCHEDULER_TARGET_LATENCY * 1)
const bigtime_t MIN_UNWEIGHTED_NORM_WORK_FOR_MIGRATION = 1000;
static const bigtime_t TARGET_CPU_IDLE_BONUS_LB = SCHEDULER_TARGET_LATENCY;
static const bigtime_t TARGET_QUEUE_PENALTY_FACTOR_LB = SCHEDULER_MIN_GRANULARITY / 2;

bool
select_load_balance_cpus(CoreEntry* sourceCore, CoreEntry* targetCore,
	CoreEntry*& finalTargetCore, CPUEntry*& sourceCPU,
	CPUEntry* idleTargetCPU)
{
	finalTargetCore = targetCore;
	sourceCPU = _scheduler_select_cpu_on_core(sourceCore, true, NULL);
	return sourceCPU != NULL;
}


static int32
scheduler_get_bl_aware_load_difference_threshold(CoreEntry* sourceCore, CoreEntry* targetCore)
{
	const int32 baseThreshold = kLoadDifference;
	int32 adjustedThreshold = baseThreshold;

	if (sourceCore == NULL || targetCore == NULL)
		return baseThreshold;

	scheduler_core_type sourceType = sourceCore->Type();
	scheduler_core_type targetType = targetCore->Type();

	if (sourceType == CORE_TYPE_LITTLE && (targetType == CORE_TYPE_BIG || targetType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
		adjustedThreshold = baseThreshold * 3 / 4;
	}
	else if ((sourceType == CORE_TYPE_BIG || sourceType == CORE_TYPE_UNIFORM_PERFORMANCE) && targetType == CORE_TYPE_LITTLE) {
		adjustedThreshold = baseThreshold * 5 / 4;
	}

	adjustedThreshold = max_c(baseThreshold / 2, adjustedThreshold);
	adjustedThreshold = min_c(baseThreshold * 3 / 2, adjustedThreshold);

	TRACE_SCHED_BL("BLDiffThreshold: Source (T%d, C%u) Target (T%d, C%u) -> Base: %d, Adjusted: %d\n",
		sourceType, sourceCore->PerformanceCapacity(),
		targetType, targetCore->PerformanceCapacity(),
		baseThreshold, adjustedThreshold);

	return adjustedThreshold;
}


static bool
find_load_balance_cores(CoreEntry*& sourceCore, CoreEntry*& targetCore)
{
	sourceCore = NULL;
	targetCore = NULL;
	int32 maxLoadFound = -1;
	int32 minLoadFound = 0x7fffffff;

	for (int32 shardIdx = 0; shardIdx < Scheduler::kNumCoreLoadHeapShards; shardIdx++) {
		ReadSpinLocker shardLocker(gCoreHeapsShardLock[shardIdx]);
		CoreEntry* shardBestSource = Scheduler::gCoreHighLoadHeapShards[shardIdx].PeekMinimum();
		if (shardBestSource != NULL && !shardBestSource->IsDefunct() && shardBestSource->GetLoad() > maxLoadFound) {
			maxLoadFound = shardBestSource->GetLoad();
			sourceCore = shardBestSource;
		}

		CoreEntry* shardBestTarget = Scheduler::gCoreLoadHeapShards[shardIdx].PeekMinimum();
		if (shardBestTarget != NULL && !shardBestTarget->IsDefunct() && shardBestTarget->GetLoad() < minLoadFound) {
			if (sourceCore != NULL && shardBestTarget == sourceCore) {
				CoreEntry* nextBestTarget = Scheduler::gCoreLoadHeapShards[shardIdx].PeekMinimum(1);
				if (nextBestTarget != NULL && !nextBestTarget->IsDefunct() && nextBestTarget->GetLoad() < minLoadFound) {
					minLoadFound = nextBestTarget->GetLoad();
					targetCore = nextBestTarget;
				}
			} else {
				minLoadFound = shardBestTarget->GetLoad();
				targetCore = shardBestTarget;
			}
		}
		shardLocker.Unlock();
	}

	if (sourceCore == NULL || targetCore == NULL || sourceCore == targetCore) {
		if (sourceCore != NULL && targetCore == sourceCore) {
			minLoadFound = 0x7fffffff;
			CoreEntry* alternativeTarget = NULL;
			for (int32 i = 0; i < gCoreCount; ++i) {
				CoreEntry* core = &gCoreEntries[i];
				if (core->IsDefunct() || core == sourceCore) continue;
				if (core->GetLoad() < minLoadFound) {
					minLoadFound = core->GetLoad();
					alternativeTarget = core;
				}
			}
			targetCore = alternativeTarget;
		}
		if (sourceCore == NULL || targetCore == NULL || sourceCore == targetCore)
			return false;
	}
	return true;
}

static bool
select_thread_to_migrate(CPUEntry* sourceCPU, CoreEntry* finalTargetCore,
	CPUEntry* idleTargetCPUOnTargetCore, Scheduler::ThreadData*& threadToMove)
{
	bigtime_t now = system_time();

	sourceCPU->LockRunQueue();
	EevdfScheduler& sourceQueue = sourceCPU->GetEevdfScheduler();

	Scheduler::ThreadData* bestCandidateToMove = NULL;
	bigtime_t maxBenefitScore = -1;

	const int MAX_LB_CANDIDATES_TO_CHECK = 10;

	Scheduler::ThreadData* tempStorage[MAX_LB_CANDIDATES_TO_CHECK] = { NULL };
	int checkedCount = 0;

	for (int i = 0; i < MAX_LB_CANDIDATES_TO_CHECK && !sourceQueue.IsEmpty(); ++i) {
		struct thread* candidateThread = sourceQueue.PopMinThread();
		if (candidateThread == NULL) break;
		Scheduler::ThreadData* candidate = (Scheduler::ThreadData*)candidateThread->scheduler_data;
		tempStorage[checkedCount++] = candidate;

		if (candidate->IsIdle() ||
			candidate->GetThread() == gCPU[sourceCPU->ID()].running_thread ||
			candidate->GetThread()->pinned_to_cpu != 0 ||
			(now - candidate->LastMigrationTime() < kMinTimeBetweenMigrations)) {
			continue;
		}

		int32 candidateWeightForLagCheck = scheduler_priority_to_weight(candidate->GetThread(), sourceCPU);
		if (candidateWeightForLagCheck <= 0) candidateWeightForLagCheck = 1;
		bigtime_t unweightedNormWorkOwed = (candidate->Lag() * candidateWeightForLagCheck) / SCHEDULER_WEIGHT_SCALE;

		if (unweightedNormWorkOwed < MIN_UNWEIGHTED_NORM_WORK_FOR_MIGRATION) {
			TRACE_SCHED_LB("LoadBalance: Candidate T %" B_PRId32 " unweighted_norm_work_owed %" B_PRId64 " < threshold %" B_PRId64 ". Skipping.\n",
				candidate->GetThread()->id, unweightedNormWorkOwed, MIN_UNWEIGHTED_NORM_WORK_FOR_MIGRATION);
			continue;
		}

		Scheduler::CPUEntry* representativeTargetCPU = _scheduler_select_cpu_on_core(finalTargetCore, false, candidate);
		if (representativeTargetCPU == NULL) representativeTargetCPU = sourceCPU;

		bigtime_t targetQueueMinVruntime = representativeTargetCPU->GetCachedMinVirtualRuntime();
		bigtime_t estimatedVRuntimeOnTarget = max_c(candidate->VirtualRuntime(), targetQueueMinVruntime);

		int32 candidateWeight = scheduler_priority_to_weight(candidate->GetThread(), sourceCPU);
		if (candidateWeight <= 0) candidateWeight = 1;

		bigtime_t candidateSliceDuration = candidate->SliceDuration();
		uint32 targetCoreCapacity = finalTargetCore->PerformanceCapacity() > 0 ? finalTargetCore->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
		uint64 normalizedSliceWorkNum = (uint64)candidateSliceDuration * targetCoreCapacity;
		bigtime_t normalizedSliceWorkOnTarget = normalizedSliceWorkNum / SCHEDULER_NOMINAL_CAPACITY;
		bigtime_t weightedNormalizedSliceEntitlementOnTarget = (normalizedSliceWorkOnTarget * SCHEDULER_WEIGHT_SCALE) / candidateWeight;
		bigtime_t estimatedLagOnTarget = weightedNormalizedSliceEntitlementOnTarget - (estimatedVRuntimeOnTarget - targetQueueMinVruntime);
		bigtime_t estimatedEligibleTimeOnTarget;
		if (estimatedLagOnTarget >= 0) {
			estimatedEligibleTimeOnTarget = now;
		} else {
			uint64 delayNumerator = (uint64)(-estimatedLagOnTarget) * candidateWeight * SCHEDULER_NOMINAL_CAPACITY;
			uint64 delayDenominator = (uint64)SCHEDULER_WEIGHT_SCALE * targetCoreCapacity;
			bigtime_t wallClockDelay = (delayDenominator == 0) ? SCHEDULER_TARGET_LATENCY * 2 : delayNumerator / delayDenominator;
			wallClockDelay = min_c(wallClockDelay, (bigtime_t)SCHEDULER_TARGET_LATENCY * 2);
			estimatedEligibleTimeOnTarget = now + max_c(wallClockDelay, (bigtime_t)SCHEDULER_MIN_GRANULARITY);
		}

		bigtime_t lagNormUnweightedOnSource = unweightedNormWorkOwed;
		uint32 sourceCoreTrueCapacity = sourceCPU->Core()->PerformanceCapacity();
		if (sourceCoreTrueCapacity == 0) {
			TRACE_SCHED_WARNING("LoadBalance: Source Core %" B_PRId32 " has 0 capacity! Using nominal %u for lag_wall_clock calc.\n",
				sourceCPU->Core()->ID(), SCHEDULER_NOMINAL_CAPACITY);
			sourceCoreTrueCapacity = SCHEDULER_NOMINAL_CAPACITY;
		}
		bigtime_t lagWallClockOnSource = 0;
		if (sourceCoreTrueCapacity > 0) {
		    lagWallClockOnSource = (lagNormUnweightedOnSource * SCHEDULER_NOMINAL_CAPACITY) / sourceCoreTrueCapacity;
		} else {
		    lagWallClockOnSource = SCHEDULER_TARGET_LATENCY * 10;
		    TRACE_SCHED_WARNING("LoadBalance: Source Core %" B_PRId32 " capacity is zero after nominal fallback. Using large fallback lag.\n", sourceCPU->Core()->ID());
		}
		bigtime_t eligibilityImprovementWallClock = candidate->EligibleTime() - estimatedEligibleTimeOnTarget;
		bool taskIsPCritical = (candidate->GetBasePriority() >= B_URGENT_DISPLAY_PRIORITY
			|| candidate->GetLoad() > (kMaxLoad * 7 / 10));
		bool taskIsEPreferring = (!taskIsPCritical
			&& (candidate->GetBasePriority() < B_NORMAL_PRIORITY
				|| candidate->GetLoad() < (kMaxLoad / 5)));
		scheduler_core_type sourceType = sourceCPU->Core()->Type();
		scheduler_core_type targetType = finalTargetCore->Type();
		bigtime_t typeCompatibilityBonus = 0;
		const bigtime_t P_TO_E_PENALTY_HIGH_LOAD_SOURCE = SCHEDULER_TARGET_LATENCY * 12;
		const bigtime_t P_TO_E_PENALTY_DEFAULT = SCHEDULER_TARGET_LATENCY * 6;
		const bigtime_t E_TO_P_BONUS_PCRITICAL = SCHEDULER_TARGET_LATENCY * 8;
		const bigtime_t E_TO_P_BONUS_DEFAULT = SCHEDULER_TARGET_LATENCY * 2;
		const bigtime_t P_TO_E_BONUS_EPREF_PS = SCHEDULER_TARGET_LATENCY * 4;

		if (gSchedulerLoadBalancePolicy == Scheduler::SPREAD) {
			if (taskIsPCritical) {
				if (sourceType == CORE_TYPE_LITTLE && (targetType == CORE_TYPE_BIG || targetType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
					typeCompatibilityBonus += E_TO_P_BONUS_PCRITICAL;
				} else if ((sourceType == CORE_TYPE_BIG || sourceType == CORE_TYPE_UNIFORM_PERFORMANCE) && targetType == CORE_TYPE_LITTLE) {
						if (sourceCPU->Core()->GetLoad() < (int32)(kVeryHighLoad * sourceCPU->Core()->PerformanceCapacity() / SCHEDULER_NOMINAL_CAPACITY))
						typeCompatibilityBonus -= P_TO_E_PENALTY_HIGH_LOAD_SOURCE;
					else
						typeCompatibilityBonus -= P_TO_E_PENALTY_DEFAULT;
				}
			} else {
				if (sourceType == CORE_TYPE_LITTLE && (targetType == CORE_TYPE_BIG || targetType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
					typeCompatibilityBonus += E_TO_P_BONUS_DEFAULT / 2;
				}
			}
		} else {
			if (taskIsEPreferring) {
				if ((sourceType == CORE_TYPE_BIG || sourceType == CORE_TYPE_UNIFORM_PERFORMANCE) && targetType == CORE_TYPE_LITTLE) {
					typeCompatibilityBonus += P_TO_E_BONUS_EPREF_PS;
				} else if (sourceType == CORE_TYPE_LITTLE && (targetType == CORE_TYPE_BIG || targetType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
					if (finalTargetCore->GetLoad() > kLowLoad / 2)
						typeCompatibilityBonus -= SCHEDULER_TARGET_LATENCY;
				}
			} else if (taskIsPCritical) {
				if (sourceType == CORE_TYPE_LITTLE && (targetType == CORE_TYPE_BIG || targetType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
					typeCompatibilityBonus += E_TO_P_BONUS_DEFAULT;
				} else if ((sourceType == CORE_TYPE_BIG || sourceType == CORE_TYPE_UNIFORM_PERFORMANCE) && targetType == CORE_TYPE_LITTLE) {
					typeCompatibilityBonus -= P_TO_E_PENALTY_DEFAULT;
				}
			}
		}
		TRACE_SCHED_BL("LoadBalance: Task T%" B_PRId32 " (Pcrit:%d, EPref:%d) from CoreType %d to %d. TypeBonus: %" B_PRId64 "\n",
			candidate->GetThread()->id, taskIsPCritical, taskIsEPreferring, sourceType, targetType, typeCompatibilityBonus);

		bigtime_t affinityBonusWallClock = 0;
		if (idleTargetCPUOnTargetCore != NULL
			&& candidate->GetThread()->previous_cpu == &gCPU[idleTargetCPUOnTargetCore->ID()]) {
			affinityBonusWallClock = SCHEDULER_TARGET_LATENCY * 2;
			TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " gets wake-affinity bonus %" B_PRId64 " for CPU %" B_PRId32 "\n",
				candidate->GetThread()->id, affinityBonusWallClock, idleTargetCPUOnTargetCore->ID());
		}

		bigtime_t targetCpuIdleBonus = 0;
		if (representativeTargetCPU != NULL && representativeTargetCPU->IsEffectivelyIdle()) {
			targetCpuIdleBonus = TARGET_CPU_IDLE_BONUS_LB;
			TRACE_SCHED_BL("LoadBalance: Candidate T %" B_PRId32 ", target CPU %" B_PRId32 " is idle. Adding idle bonus %" B_PRId64 ".\n",
				candidate->GetThread()->id, representativeTargetCPU->ID(), targetCpuIdleBonus);
		}

		bigtime_t currentBenefitScore = (kBenefitScoreLagFactor * lagWallClockOnSource)
									  + (kBenefitScoreEligFactor * eligibilityImprovementWallClock)
									  + typeCompatibilityBonus
									  + affinityBonusWallClock
									  + targetCpuIdleBonus
									  - (0 > 0 ? 0 : 0);


		bigtime_t queueDepthPenalty = 0;
		if (representativeTargetCPU != NULL) {
			int32 targetQueueDepth = representativeTargetCPU->GetEevdfScheduler().Count();
			if (targetQueueDepth > 0) {
				queueDepthPenalty = - (targetQueueDepth * TARGET_QUEUE_PENALTY_FACTOR_LB);
				currentBenefitScore += queueDepthPenalty;
				TRACE_SCHED_BL("LoadBalance: Candidate T %" B_PRId32 ", target CPU %" B_PRId32 " has queue depth %" B_PRId32 ". Adding penalty %" B_PRId64 ".\n",
					candidate->GetThread()->id, representativeTargetCPU->ID(), targetQueueDepth, queueDepthPenalty);
			}
		}

		if (candidate->IsLikelyIOBound() && affinityBonusWallClock == 0 && targetCpuIdleBonus == 0) {
			if (representativeTargetCPU != NULL && representativeTargetCPU->GetEevdfScheduler().Count() > 1) {
				currentBenefitScore /= kIOBoundScorePenaltyFactor;
				TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " is likely I/O bound (no affinity/idle target, target queue > 1), reducing benefit score to %" B_PRId64 " using factor %" B_PRId32 "\n",
					candidate->GetThread()->id, currentBenefitScore, kIOBoundScorePenaltyFactor);
			} else if (representativeTargetCPU == NULL || representativeTargetCPU->GetEevdfScheduler().Count() <=1) {
				TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " is likely I/O bound but target queue is short or no other bonus, I/O penalty not applied this time.\n", candidate->GetThread()->id);
			}
		} else if (candidate->IsLikelyIOBound() && (affinityBonusWallClock != 0 || targetCpuIdleBonus != 0)) {
			TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " is likely I/O bound but has wake-affinity or target is idle, I/O penalty not applied.\n",
				candidate->GetThread()->id);
		}

		TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 ": lag_wall_src %" B_PRId64 ", elig_impr %" B_PRId64 ", type_bonus %" B_PRId64 ", aff_bonus %" B_PRId64 ", idle_bonus %" B_PRId64 ", q_penalty %" B_PRId64 " -> final_score %" B_PRId64 "\n",
			candidate->GetThread()->id, lagWallClockOnSource, eligibilityImprovementWallClock,
			typeCompatibilityBonus, affinityBonusWallClock, targetCpuIdleBonus, queueDepthPenalty,
			currentBenefitScore);

		if (currentBenefitScore > maxBenefitScore) {
			bool isTaskActuallyPCritical = (candidate->GetBasePriority() >= B_URGENT_DISPLAY_PRIORITY);
			if (isTaskActuallyPCritical && (targetType == CORE_TYPE_LITTLE) && (sourceType == CORE_TYPE_BIG || sourceType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
				if (currentBenefitScore < SCHEDULER_TARGET_LATENCY) {
					TRACE_SCHED_BL("LoadBalance: Candidate T %" B_PRId32 " is P-Critical. Suppressing move from P-Core %" B_PRId32 " to E-Core %" B_PRId32 " due to insufficient benefit score %" B_PRId64 " (threshold %" B_PRId64 ").\n",
						candidate->GetThread()->id, sourceCPU->Core()->ID(), finalTargetCore->ID(), currentBenefitScore, SCHEDULER_TARGET_LATENCY);
					continue;
				}
			}
			maxBenefitScore = currentBenefitScore;
			bestCandidateToMove = candidate;
		}
	}

	for (int i = 0; i < checkedCount; ++i) {
		if (tempStorage[i] != bestCandidateToMove) {
			sourceQueue.AddThread(tempStorage[i]);
		}
	}
	threadToMove = bestCandidateToMove;

	if (threadToMove == NULL) {
		sourceCPU->UnlockRunQueue();
		TRACE("LoadBalance (EEVDF): No suitable thread found to migrate from CPU %" B_PRId32 "\n", sourceCPU->ID());
		return false;
	}
	return true;
}

static bool
scheduler_perform_load_balance(void)
{
	SCHEDULER_ENTER_FUNCTION();
	bool migrationPerformed = false;

	if (gCurrentMode != NULL && gCurrentMode->attempt_proactive_stc_designation != NULL
		&& Scheduler::sSmallTaskCore == NULL) {
		ReadSpinLocker idlePackageLocker(gIdlePackageLock);
		bool systemActive = gIdlePackageList.Count() < gPackageCount;
		idlePackageLocker.Unlock();
		if (systemActive) {
			CoreEntry* designatedCore = gCurrentMode->attempt_proactive_stc_designation();
			if (designatedCore != NULL) {
				TRACE("scheduler_load_balance_event: Proactively designated core %" B_PRId32 " as STC.\n", designatedCore->ID());
			} else {
				TRACE("scheduler_load_balance_event: Proactive STC designation attempt did not set an STC.\n");
			}
		}
	}

	if (gSingleCore || gCoreCount < 2) {
		return migrationPerformed;
	}

	CoreEntry* sourceCoreCandidate = NULL;
	CoreEntry* targetCoreCandidate = NULL;
	if (!find_load_balance_cores(sourceCoreCandidate, targetCoreCandidate))
		return migrationPerformed;

	TRACE_SCHED_BL("LoadBalance: Initial candidates: SourceCore %" B_PRId32 " (Type %d, Load %" B_PRId32 "), TargetCore %" B_PRId32 " (Type %d, Load %" B_PRId32 ")\n",
		sourceCoreCandidate->ID(), sourceCoreCandidate->Type(), sourceCoreCandidate->GetLoad(),
		targetCoreCandidate->ID(), targetCoreCandidate->Type(), targetCoreCandidate->GetLoad());

	int32 blAwareLoadDifference = scheduler_get_bl_aware_load_difference_threshold(sourceCoreCandidate, targetCoreCandidate);
	if (sourceCoreCandidate->GetLoad() <= targetCoreCandidate->GetLoad() + blAwareLoadDifference) {
		TRACE_SCHED_BL("LoadBalance: No imbalance. SourceCore %" B_PRId32 " (load %" B_PRId32 ") vs TargetCore %" B_PRId32 " (load %" B_PRId32 "). Threshold: %" B_PRId32 "\n",
			sourceCoreCandidate->ID(), sourceCoreCandidate->GetLoad(),
			targetCoreCandidate->ID(), targetCoreCandidate->GetLoad(), blAwareLoadDifference);
		return migrationPerformed;
	}

	TRACE("LoadBalance (EEVDF): Potential imbalance. SourceCore %" B_PRId32 " (load %" B_PRId32 ") TargetCore %" B_PRId32 " (load %" B_PRId32 "). Threshold: %" B_PRId32 "\n",
		sourceCoreCandidate->ID(), sourceCoreCandidate->GetLoad(),
		targetCoreCandidate->ID(), targetCoreCandidate->GetLoad(), blAwareLoadDifference);

	Scheduler::CPUEntry* sourceCPU = NULL;
	Scheduler::CPUEntry* targetCPU = NULL;
	CoreEntry* finalTargetCore = NULL;

	Scheduler::CPUEntry* idleTargetCPUOnTargetCore = _find_idle_cpu_on_core(targetCoreCandidate);
	if (idleTargetCPUOnTargetCore != NULL) {
		TRACE_SCHED("LoadBalance: TargetCore %" B_PRId32 " has an idle CPU: %" B_PRId32 "\n",
			targetCoreCandidate->ID(), idleTargetCPUOnTargetCore->ID());
	}

	if (!select_load_balance_cpus(sourceCoreCandidate, targetCoreCandidate,
			finalTargetCore, sourceCPU, idleTargetCPUOnTargetCore)) {
		return migrationPerformed;
	}

	if (sourceCPU == NULL) {
		TRACE("LoadBalance (EEVDF): Could not select a source CPU on core %" B_PRId32 ".\n", sourceCoreCandidate->ID());
		return false;
	}

	Scheduler::ThreadData* threadToMove = NULL;
	if (!select_thread_to_migrate(sourceCPU, finalTargetCore,
			idleTargetCPUOnTargetCore, threadToMove)) {
		return migrationPerformed;
	}

	targetCPU = _scheduler_select_cpu_on_core(finalTargetCore, false, threadToMove);
	if (targetCPU == NULL || targetCPU == sourceCPU) {
		if (threadToMove != NULL) {
			sourceCPU->GetEevdfScheduler().AddThread((::ThreadData*)threadToMove);
		}
		sourceCPU->UnlockRunQueue();
		TRACE("LoadBalance (EEVDF): No suitable target CPU found for thread %" B_PRId32 " on core %" B_PRId32 " or target is source.\n",
			threadToMove->GetThread()->id, finalTargetCore->ID());
		return false;
	}

		int32 threadCount = sourceCPU->GetTotalThreadCount();
		atomic_add(&threadCount, -1);
		ASSERT(sourceCPU->GetTotalThreadCount() >=0);
	sourceCPU->_UpdateMinVirtualRuntime();

	threadToMove->MarkDequeued();
	sourceCPU->UnlockRunQueue();

	TRACE_SCHED_BL("LoadBalance (EEVDF): Migrating T %" B_PRId32 " (Lag %" B_PRId64 ", Score %" B_PRId64 ") from CPU %" B_PRId32 "(C%d,T%d) to CPU %" B_PRId32 "(C%d,T%d)\n",
		threadToMove->GetThread()->id, threadToMove->Lag(), 0LL,
		sourceCPU->ID(), sourceCPU->Core()->ID(), sourceCPU->Core()->Type(),
		targetCPU->ID(), targetCPU->Core()->ID(), targetCPU->Core()->Type());

	if (threadToMove->Core() != NULL)
		threadToMove->UnassignCore(false);

	threadToMove->GetThread()->previous_cpu = &gCPU[targetCPU->ID()];
	CoreEntry* actualFinalTargetCore = targetCPU->Core();
	threadToMove->ChooseCoreAndCPU(actualFinalTargetCore, targetCPU);
	ASSERT(threadToMove->Core() == actualFinalTargetCore);

	{
		InterruptsSpinLocker schedulerLocker(threadToMove->GetThread()->scheduler_lock);
		threadToMove->UpdateEevdfParameters(targetCPU, true, false);
	}

	TRACE_SCHED("LoadBalance: Migrated T %" B_PRId32 " to CPU %" B_PRId32 " (after UpdateEevdfParameters), new VD %" B_PRId64 ", Lag %" B_PRId64 ", VRun %" B_PRId64 ", Elig %" B_PRId64 "\n",
		threadToMove->GetThread()->id, targetCPU->ID(), threadToMove->VirtualDeadline(), threadToMove->Lag(), threadToMove->VirtualRuntime(), threadToMove->EligibleTime());

	targetCPU->LockRunQueue();
	targetCPU->AddThread(threadToMove);
	targetCPU->UnlockRunQueue();

	threadToMove->SetLastMigrationTime(system_time());
	T(MigrateThread(threadToMove->GetThread(), sourceCPU->ID(), targetCPU->ID()));
	migrationPerformed = true;

	if (threadToMove->Core() != sourceCPU->Core()) {
		int32 localIrqList[4];
		int8 localIrqCount = 0;
		thread_id migratedThId = threadToMove->GetThread()->id;

		{
			InterruptsSpinLocker followTaskLocker(threadToMove->GetThread()->scheduler_lock);
			const int32* affinitizedIrqsPtr = threadToMove->GetAffinitizedIrqs(localIrqCount);
			if (localIrqCount > 0) {
				if (affinitizedIrqsPtr == NULL) {
					// This should not happen, but if it does, we should handle it gracefully.
					localIrqCount = 0;
				} else {
					memcpy(localIrqList, affinitizedIrqsPtr, localIrqCount * sizeof(int32));
				}
			}
		}

		if (localIrqCount > 0) {
			scheduler_maybe_follow_task_irqs(migratedThId, localIrqList, localIrqCount, targetCPU->Core(), targetCPU);
		}
	}

	Thread* currentOnTarget = gCPU[targetCPU->ID()].running_thread;
	Scheduler::ThreadData* currentOnTargetData = currentOnTarget ? currentOnTarget->scheduler_data : NULL;
	bool newThreadIsEligible = (system_time() >= threadToMove->EligibleTime());

	if (newThreadIsEligible && (currentOnTarget == NULL || thread_is_idle_thread(currentOnTarget) ||
		(currentOnTargetData != NULL && threadToMove->VirtualDeadline() < currentOnTargetData->VirtualDeadline()))) {
		if (targetCPU->ID() == smp_get_current_cpu()) {
			gCPU[targetCPU->ID()].invoke_scheduler = true;
		} else {
			smp_send_ici(targetCPU->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	}
	return migrationPerformed;
}
