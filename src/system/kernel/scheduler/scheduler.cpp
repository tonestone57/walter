/*
 * Copyright 2013-2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 * Audit fixes applied 2025.
 * Copyright 2009, Rene Gollent, rene@gollent.com.
 * Copyright 2008-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2010, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2002, Angelo Mottola, a.mottola@libero.it.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


/*! The thread scheduler */


#include <OS.h>

#include <algorithm>

#include <AutoDeleter.h>
#include <cpu.h>
#include <debug.h>
#include <interrupts.h>
#include <kernel.h>
#include <kscheduler.h>
#include <listeners.h>
#include <load_tracking.h>
#include <scheduler_defs.h>
#include <smp.h>
#include <timer.h>
#include <util/Random.h>
#include <slab/Slab.h>

#include <DPC.h>

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_locking.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"
#include "scheduler_topology.h"
#include "scheduler_tracing.h"


namespace Scheduler {


class ThreadEnqueuer : public ThreadProcessing {
public:
	void		operator()(ThreadData* thread);
};

scheduler_mode Scheduler::sCurrentModeID;
scheduler_mode_operations* Scheduler::sCurrentMode;

bool gSingleCore;
bool gTrackCoreLoad;
bool gTrackCPULoad;
int32 gRandomSamples;

int64 gDeadlineBucketSize __attribute__((aligned(8))) = 5000;

CoreType gMinCoreType = CORE_TYPE_UNKNOWN;
CoreType gMaxCoreType = CORE_TYPE_UNKNOWN;

bool gHasStandardCores = false;

int32 gTotalRunnableThreads = 0;
uint64 gIdleMask __attribute__((aligned(8))) = 0;

spinlock gSchedulerLock = B_SPINLOCK_INITIALIZER;


static timer sInteractionTimer;
static int64 sLastInteractionTime __attribute__((aligned(8)));
static int32 sDPCPending = 0;
static int32 sTimerArmed = 0;
static int32 sPendingDPCTarget = 0;


static SchedulerSnapshot TakeSnapshot()
{
	return MakeSchedulerSnapshot(gTotalRunnableThreads, gIdleMask);
}

static const int kLoadBalanceThreshold = 2;
static const bigtime_t kRescheduleCooldown = 500;


extern "C" void
AcquireSchedulerSpinlock()
{
	acquire_spinlock(&gSchedulerLock);
}


extern "C" void
ReleaseSchedulerSpinlock()
{
	release_spinlock(&gSchedulerLock);
}


static void
UpdateDeadlineScalingScalable()
{
	ThreadData::ComputeQuantumLengths();
}


static void
update_quantum_lengths_dpc(void* /*arg*/)
{
	int64 targetResolution = (int64)atomic_get(&sPendingDPCTarget);

	{
		InterruptsBigSchedulerLocker locker;
		if (atomic_get64(&gDeadlineBucketSize) != targetResolution) {
			atomic_set64(&gDeadlineBucketSize, targetResolution);
			UpdateDeadlineScalingScalable();
		}
	}

	atomic_set(&sDPCPending, 0);
}


static status_t
interaction_timer_hook(struct timer* timer)
{
	atomic_set(&sTimerArmed, 0);

	atomic_set(&sPendingDPCTarget, 5000);
	if (atomic_get_and_set(&sDPCPending, 1) == 0) {
		int64 target = (int64)atomic_get(&sPendingDPCTarget);
		if (DPCQueue::DefaultQueue(B_URGENT_DISPLAY_PRIORITY)->Add(
				&update_quantum_lengths_dpc, (void*)(addr_t)target) != B_OK) {
			atomic_set(&sDPCPending, 0);
			atomic_set(&sPendingDPCTarget, 0);
		}
	}

	return B_HANDLED_INTERRUPT;
}


void
scheduler_update_interaction_state()
{
	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());

	if (cpu->fInteractionUpdateCounter++ % 32 != 0)
		return;

	int64 currentBucketSize = atomic_get64(&gDeadlineBucketSize);

	bigtime_t now = system_time();
	bigtime_t lastTime = atomic_get64(&sLastInteractionTime);
	// Issue 97 fix: cache the current mode pointer locally to prevent
	// producing garbage thresholds from mode-switch tearing on 32-bit platforms.
	scheduler_mode_operations* const snapMode = Scheduler::GetCurrentMode();
	bigtime_t threshold = (snapMode != NULL) ? snapMode->minimal_quantum
		: 1200;

	while (now - lastTime >= threshold) {
		if (atomic_test_and_set64(&sLastInteractionTime, now, lastTime) == lastTime) {
			lastTime = now;
			break;
		}

		lastTime = atomic_get64(&sLastInteractionTime);
		if (now - lastTime < threshold)
			return;
	}

	if (currentBucketSize == 1000) {
		if (atomic_get_and_set(&sTimerArmed, 1) == 0) {
			add_timer(&sInteractionTimer, &interaction_timer_hook, 500000,
				B_ONE_SHOT_RELATIVE_TIMER);
		}
		return;
	}

	atomic_set(&sPendingDPCTarget, 1000);
	if (atomic_get_and_set(&sDPCPending, 1) == 0) {
		int64 target = (int64)atomic_get(&sPendingDPCTarget);
		if (DPCQueue::DefaultQueue(B_URGENT_DISPLAY_PRIORITY)->Add(
				&update_quantum_lengths_dpc, (void*)(addr_t)target) != B_OK) {
			atomic_set(&sDPCPending, 0);
			atomic_set(&sPendingDPCTarget, 0);
			return;
		}
	}

	if (atomic_get_and_set(&sTimerArmed, 1) == 0) {
		add_timer(&sInteractionTimer, &interaction_timer_hook, 500000,
			B_ONE_SHOT_RELATIVE_TIMER);
	}
}



struct RunQueueScanner {
		uint32 kTopWordMask;
		int kMaxThreadsToCheckPerQueue;

		RunQueueScanner(uint32 topWordMask, int maxThreads)
			: kTopWordMask(topWordMask), kMaxThreadsToCheckPerQueue(maxThreads) {}

		void operator()(const ThreadRunQueue* runQueue) const {
			const uint32* bitmap = runQueue->GetBitmap();

			for (int i = ThreadRunQueue::kBitmapSize - 1; i >= 0; i--) {
				uint32 val = bitmap[i];

				if (i == ThreadRunQueue::kBitmapSize - 1)
					val &= kTopWordMask;

				if (val == 0)
					continue;

				int bit = fls(val) - 1;
				while (true) {
					unsigned int priority = i * 32 + bit;
					ThreadData* thread = runQueue->GetHead(priority);
					int count = 0;

					while (thread != NULL && count++ < kMaxThreadsToCheckPerQueue) {
						ThreadData* next = thread->GetRunQueueLink()->fNext;
						thread->_UpdatePriorityBoost();
						thread = next;
					}

					val &= ~(1UL << bit);
					if (val == 0)
						break;
					bit = fls(val) - 1;
				}
			}
		}
	};


struct TopologyComparator {
		bool distinctTopology;
		TopologyComparator(bool distinct) : distinctTopology(distinct) {}

		int32 GetTopoKey(int32 cpu) const
		{
			return distinctTopology ? get_topology_id(cpu) : (cpu / 16);
		}

		bool operator()(int32 a, int32 b) const
		{
			int32 topoA = GetTopoKey(a);
			int32 topoB = GetTopoKey(b);
			if (topoA != topoB)
				return topoA < topoB;
			return a < b;
		}
	};




static int32 sSchedulerEnabled;

SchedulerListenerList gSchedulerListeners;
rw_spinlock gSchedulerListenersLock = B_RW_SPINLOCK_INITIALIZER;

static scheduler_mode_operations* sSchedulerModes[] = {
	&gSchedulerLowLatencyMode,
	&gSchedulerPowerSavingMode,
};

static object_cache* sThreadDataCache;

static int32* sCPUToCore;
static int32* sCPUToPackage;
static int32* sPackageToNode;
static int32* sCPUToCluster = NULL;


static void
UpdatePriorityBoostScalable(CoreEntry* core, CPUEntry* cpu)
{
	SCHEDULER_ENTER_FUNCTION();

	static const uint32 kTopWordMask =
		(uint32)((2ULL << (THREAD_MAX_SET_PRIORITY % 32)) - 1);

	if (cpu->fRescheduleCount++ % 10 != 0)
		return;

	const int kMaxThreadsToCheckPerQueue = 5;

	RunQueueScanner scanRunQueue(kTopWordMask, kMaxThreadsToCheckPerQueue);

	int32 coreCPUCount = max_c(1, core->CPUCount());
	uint32 preCount = cpu->fRescheduleCount - 1;
	if (cpu->fRescheduleCount == 0)
		preCount = THREAD_MAX_SET_PRIORITY;

	uint32 boostEpoch = preCount / 10;

	// Use scheduler_popcount to map bitmask-based indices to a dense range.
	native_cpu_mask_t indices = scheduler_atomic_get(&core->fLocalIndices);
	int32 denseIndex = scheduler_popcount(indices
		& (((native_cpu_mask_t)1 << cpu->fCoreLocalIndex) - 1));

	bool ownsCoreQueueScan =
		((int32)(boostEpoch % (uint32)coreCPUCount)
			== (int32)(denseIndex % (uint32)coreCPUCount));

	CoreRunQueueLocker coreLocker(core, false, false);
	if (ownsCoreQueueScan) {
		coreLocker.Lock();
		if (core->CoreRunQueueThreadCount() > 0)
			scanRunQueue(core->RunQueue());
	}

	if (cpu->ThreadCount() > 0) {
		CPURunQueueLocker cpuLocker(cpu);
		scanRunQueue(cpu->RunQueue());
	}
}


static bool enqueue(Thread* thread, bool newOne, Thread* waker);


void
ThreadEnqueuer::operator()(ThreadData* thread)
{
	enqueue(thread->GetThread(), false, NULL);
}


void
scheduler_dump_thread_data(Thread* thread)
{
	thread->scheduler_data->Dump();
}





static bool
enqueue(Thread* thread, bool newOne, Thread* waker)
{
	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;

	int32 threadPriority = threadData->GetEffectivePriority();
	T(EnqueueThread(thread, threadPriority));

	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;
	if (thread->pinned_to_cpu > 0) {
		int32 pinnedCPU = thread->pinned_to_cpu - 1;
		targetCPU = CPUEntry::GetCPU(pinnedCPU);
		if (gCPU[targetCPU->ID()].disabled)
			targetCPU = NULL;
	} else if (gSingleCore) {
		targetCore = &gCoreEntries[0];
	} else if (waker != NULL) {
		ThreadData* wakerData = waker->scheduler_data;
		CoreEntry* wakerCore = (wakerData != NULL) ? wakerData->Core() : NULL;
		if (wakerCore != NULL && wakerCore->CPUCount() > 0)
			targetCore = wakerCore;
	} else if (threadData->Core() != NULL
		&& (!newOne || !threadData->HasCacheExpired())) {
		targetCore = threadData->Rebalance();
	}

	bool wasRunQueueEmpty = false;
	bool requestPreemption = false;
	bool rescheduleNeeded = false;
	bool updateInteraction = false;

	const int32 kMaxRetries = smp_get_num_cpus() * 2 + 8;
	int32 enqueueAttempts = 0;
	do {
		rescheduleNeeded = threadData->ChooseCoreAndCPU(targetCore, targetCPU);

		if (!threadData->Enqueue(wasRunQueueEmpty, requestPreemption,
				updateInteraction)) {
			targetCore = NULL;
			targetCPU = NULL;
			if (++enqueueAttempts >= kMaxRetries) {
				// Issue 31: if the ThreadData::Enqueue retry loop reaches its
				// limit and gives up, gTotalRunnableThreads must be manually
				// decremented to prevent counter leaks.
				if (!threadData->IsIdle())
					atomic_add(&gTotalRunnableThreads, -1);

				return false;
			}
		} else {
			break;
		}
	} while (true);

	if (updateInteraction)
		scheduler_update_interaction_state();

	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue,
		thread);

	int32 heapPriority = CPUPriorityHeap::GetKey(targetCPU);
	if (threadPriority > heapPriority
		|| (threadPriority == heapPriority && rescheduleNeeded)
		|| wasRunQueueEmpty
		|| requestPreemption) {

		if (targetCPU->ID() == smp_get_current_cpu()) {
			gCPU[targetCPU->ID()].invoke_scheduler = true;
		} else {
			bigtime_t now = system_time();
			if (ShouldReschedule(now, targetCPU->lastReschedule, kRescheduleCooldown)) {
				if (targetCPU->SetReschedulePending()) {
					targetCPU->lastReschedule = now;
					// Issue 16/84 fix: ensure ICI is sent after successful enqueue.
					smp_send_ici(targetCPU->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0,
						NULL, SMP_MSG_FLAG_ASYNC);
				}
			}
		}
	}
	return true;
}


bool
enqueue_safe(Thread* thread)
{
	return enqueue(thread, false, NULL);
}


void
scheduler_enqueue_in_run_queue(Thread *thread)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	SchedulerModeLocker _;

	AssertThreadReady(thread);

	ThreadData* threadData = thread->scheduler_data;
	Thread* waker = thread->waker;
	thread->waker = NULL;

	threadData->ResetPriorityBoost();
	enqueue(thread, true, waker);
}


int32
scheduler_set_thread_priority(Thread *thread, int32 priority)
{
	ASSERT(are_interrupts_enabled());

	InterruptsSpinLocker _(thread->scheduler_lock);
	SchedulerModeLocker modeLocker;

	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;
	int32 oldPriority = thread->priority;

	thread->priority = priority;
	threadData->ResetPriorityBoost();

	if (priority == oldPriority)
		return oldPriority;

	if (thread->state != B_THREAD_READY) {
		if (thread->state == B_THREAD_RUNNING) {
			ASSERT(threadData->Core() != NULL);

			ASSERT(thread->cpu != NULL);
			CPUEntry* cpu = &gCPUEntries[thread->cpu->cpu_num];

			if (!gCPU[cpu->ID()].disabled) {
				CoreCPUHeapLocker _(threadData->Core());
				cpu->UpdatePriority(priority);
			}
		}

		return oldPriority;
	}

	T(RemoveThread(thread));

	NotifySchedulerListeners(&SchedulerListener::ThreadRemovedFromRunQueue,
		thread);

	if (threadData->Dequeue())
		enqueue(thread, true, NULL);

	return oldPriority;
}


void
scheduler_reschedule_ici()
{
	get_cpu_struct()->invoke_scheduler = true;
}


static inline void
stop_cpu_timers(Thread* fromThread, Thread* toThread)
{
	SpinLocker teamLocker(&fromThread->team->time_lock);
	SpinLocker threadLocker(&fromThread->time_lock);

	if (fromThread->HasActiveCPUTimeUserTimers()
		|| fromThread->team->HasActiveCPUTimeUserTimers()) {
		user_timer_stop_cpu_timers(fromThread, toThread);
	}
}


static inline void
continue_cpu_timers(Thread* thread, cpu_ent* cpu)
{
	SpinLocker teamLocker(&thread->team->time_lock);
	SpinLocker threadLocker(&thread->time_lock);

	if (thread->HasActiveCPUTimeUserTimers()
		|| thread->team->HasActiveCPUTimeUserTimers()) {
		user_timer_continue_cpu_timers(thread, cpu->previous_thread);
	}
}


static void
thread_resumes(Thread* thread)
{
	cpu_ent* cpu = thread->cpu;
	Thread* previousThread = cpu->previous_thread;

	continue_cpu_timers(thread, cpu);

	release_spinlock(&previousThread->scheduler_lock);

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


static inline void
switch_thread(Thread* fromThread, Thread* toThread)
{
	if ((fromThread->flags & THREAD_FLAGS_DEBUGGER_INSTALLED) != 0)
		user_debug_thread_unscheduled(fromThread);

	stop_cpu_timers(fromThread, toThread);

	cpu_ent* cpu = fromThread->cpu;
	toThread->previous_cpu = toThread->cpu = cpu;
	fromThread->cpu = NULL;
	cpu->running_thread = toThread;
	cpu->previous_thread = fromThread;

	arch_thread_set_current_thread(toThread);
	arch_thread_context_switch(fromThread, toThread);

	thread_resumes(fromThread);
}


static void
reschedule(int32 nextState)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	int32 thisCPU = smp_get_current_cpu();
	gCPU[thisCPU].invoke_scheduler = false;

	CPUEntry* cpu = CPUEntry::GetCPU(thisCPU);
	cpu->ClearReschedulePending();
	CoreEntry* core = CoreEntry::GetCore(thisCPU);

	Thread* oldThread = thread_get_current_thread();
	ThreadData* oldThreadData = oldThread->scheduler_data;

	CPUSet oldThreadMask;
	bool useOldThreadMask, fetchedOldThreadMask = false;

	oldThreadData->StopCPUTime();

	SchedulerModeLocker modeLocker;

	oldThread->state = nextState;

	oldThreadData->SetStolenInterruptTime(gCPU[thisCPU].interrupt_time);

	bool enqueueOldThread = false;
	bool putOldThreadAtBack = false;
	switch (nextState) {
		case B_THREAD_RUNNING:
		case B_THREAD_READY:
			enqueueOldThread = true;

			oldThreadMask = oldThreadData->GetCPUMask();
			useOldThreadMask = !oldThreadMask.IsEmpty();
			fetchedOldThreadMask = true;

			if (!oldThreadData->IsIdle() && (!useOldThreadMask || oldThreadMask.GetBit(thisCPU))) {
				oldThreadData->Continues();
				if (oldThreadData->HasQuantumEnded(oldThread->cpu->preempted,
						oldThread->has_yielded)) {
					putOldThreadAtBack = true;
				} else {
					putOldThreadAtBack = false;
				}
			}

			break;
		case THREAD_STATE_FREE_ON_RESCHED:
			oldThreadData->Dies();
			enqueueOldThread = false;
			break;
		default:
			oldThreadData->GoesAway();
			break;
	}

	oldThread->has_yielded = false;

	ThreadData* nextThreadData;
	if (gCPU[thisCPU].disabled) {
		if (!oldThreadData->IsIdle()) {
			putOldThreadAtBack = true;
			oldThreadData->UnassignCore(true);
			core->DecrementTotalThreadCount();
			cpu->UpdateActiveTime(oldThreadData);

			CPURunQueueLocker cpuLocker(cpu);
			nextThreadData = cpu->PeekIdleThread();
			cpu->Remove(nextThreadData);
		} else
			nextThreadData = oldThreadData;
	} else {
		if (!fetchedOldThreadMask) {
			oldThreadMask = oldThreadData->GetCPUMask();
			useOldThreadMask = !oldThreadMask.IsEmpty();
			fetchedOldThreadMask = true;
		}
		bool oldThreadShouldMigrate = useOldThreadMask && !oldThreadMask.GetBit(thisCPU);
		if (oldThreadShouldMigrate)
			enqueueOldThread = false;

		nextThreadData
			= cpu->ChooseNextThread(enqueueOldThread ? oldThreadData : NULL,
				putOldThreadAtBack);

		cpu->UpdateActiveTime(oldThreadData);

		if (oldThreadShouldMigrate) {
			enqueue(oldThread, true, NULL);
			if (oldThreadData == nextThreadData) {
				nextThreadData = cpu->PeekIdleThread();
				if (nextThreadData == NULL)
					nextThreadData = oldThreadData;
			}
		}

		CoreCPUHeapLocker cpuLocker(core);
		cpu->UpdatePriority(nextThreadData->GetEffectivePriority());
	}

	UpdatePriorityBoostScalable(core, cpu);

	Thread* nextThread = nextThreadData->GetThread();
	ASSERT(!gCPU[thisCPU].disabled || nextThreadData->IsIdle());

	if (nextThread != oldThread) {
		if (enqueueOldThread) {
			if (putOldThreadAtBack)
				enqueue(oldThread, false, NULL);
			else
				oldThreadData->PutBack();
		}

		acquire_spinlock(&nextThread->scheduler_lock);
	}

	T(ScheduleThread(nextThread, oldThread));

	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled,
		oldThread, nextThread);

	ASSERT(nextThreadData->Core() == core);
	nextThread->state = B_THREAD_RUNNING;
	nextThreadData->StartCPUTime();

	cpu->TrackLoad(nextThreadData);

	if (nextThread != oldThread || oldThread->cpu->preempted) {
		int32 load = core->ThreadCount();
		bigtime_t quantum = Scheduler::BaseQuantum();
		if (load > 2) {
			int32 divisor = max_c(1, load - 1);
			quantum = max_c(Scheduler::MinimalQuantum(), quantum / divisor);
		}
		nextThreadData->SetQuantum(quantum);

		cpu->StartQuantumTimer(nextThreadData, oldThread->cpu->preempted);

		oldThread->cpu->preempted = false;
		if (!nextThreadData->IsIdle())
			nextThreadData->Continues();
		else
			Scheduler::RebalanceIRQs(true);
		nextThreadData->StartQuantum();

		modeLocker.Unlock();

		SCHEDULER_EXIT_FUNCTION();

		if (nextThread != oldThread)
			switch_thread(oldThread, nextThread);
	}
}


void
scheduler_reschedule(int32 nextState)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	if (!atomic_get(&sSchedulerEnabled)) {
		Thread* thread = thread_get_current_thread();
		if (thread != NULL && nextState != B_THREAD_READY)
			panic("scheduler_reschedule_no_op() called in non-ready thread");
		return;
	}

	reschedule(nextState);
}


status_t
scheduler_on_thread_create(Thread* thread, bool idleThread)
{
	void* buffer = object_cache_alloc(sThreadDataCache, 0);
	if (buffer == NULL)
		return B_NO_MEMORY;

	thread->scheduler_data = new(buffer) ThreadData(thread);
	return B_OK;
}


void
scheduler_on_thread_init(Thread* thread)
{
	ASSERT(thread->scheduler_data != NULL);

	if (thread_is_idle_thread(thread)) {
		static int32 sIdleThreadsID;
		int32 cpuID = atomic_add(&sIdleThreadsID, 1);

		thread->previous_cpu = &gCPU[cpuID];
		thread->pinned_to_cpu = 1;

		thread->scheduler_data->Init(CoreEntry::GetCore(cpuID));
	} else
		thread->scheduler_data->Init();
}


void
scheduler_on_thread_destroy(Thread* thread)
{
	if (thread->scheduler_data != NULL) {
		thread->scheduler_data->~ThreadData();
		object_cache_free(sThreadDataCache, thread->scheduler_data);
		thread->scheduler_data = NULL;
	}
}


void
scheduler_start()
{
	InterruptsSpinLocker _(thread_get_current_thread()->scheduler_lock);
	SCHEDULER_ENTER_FUNCTION();

	reschedule(B_THREAD_READY);
}


status_t
scheduler_set_operation_mode(scheduler_mode mode)
{
	if (mode != SCHEDULER_MODE_LOW_LATENCY
		&& mode != SCHEDULER_MODE_POWER_SAVING) {
		return B_BAD_VALUE;
	}

	InterruptsBigSchedulerLocker _;

	Scheduler::SetOperationMode(mode, sSchedulerModes[mode]);
	Scheduler::SwitchToMode();

	ThreadData::ComputeQuantumLengths();

	return B_OK;
}


void
scheduler_set_cpu_enabled(int32 cpuID, bool enabled)
{
#if KDEBUG
	if (are_interrupts_enabled())
		panic("scheduler_set_cpu_enabled: called with interrupts enabled");
#endif

	Scheduler::SetCPUEnabled(cpuID, enabled);

	CPUEntry* cpu = &gCPUEntries[cpuID];
	CoreEntry* core = cpu->Core();

	ASSERT(core->CPUCount() >= 0);

	if (enabled) {
		cpu->LockScheduler();
		{
			CoreCPUHeapLocker heapLocker(core);
			cpu->Start();
			core->AddCPU(cpu);
			gCPU[cpuID].disabled = false;
			gCPUEnabled.SetBitAtomic(cpuID);
		}
		cpu->UnlockScheduler();
	} else {
		cpu->LockScheduler();

		gCPU[cpuID].disabled = true;
		gCPUEnabled.ClearBitAtomic(cpuID);

		if (core->CPUCount() == 1)
			thread_map(CoreEntry::_UnassignThread, core);

		ThreadEnqueuer enqueuer;

		while (true) {
			ThreadData* threadData;
			{
				CPURunQueueLocker locker(cpu);
				threadData = cpu->PeekThread();
				if (threadData == NULL || threadData->IsIdle())
					break;
				cpu->Remove(threadData);
			}

			enqueuer(threadData);
		}

		{
			CoreCPUHeapLocker heapLocker(core);
			cpu->UpdatePriority(B_IDLE_PRIORITY);
			core->RemoveCPU(cpu, enqueuer);
		}

		cpu->Stop();

		if (smp_get_current_cpu() != cpuID) {
			smp_send_ici(cpuID, SMP_MSG_RESCHEDULE, 0, 0, 0, NULL,
				SMP_MSG_FLAG_ASYNC);
		}

		cpu->UnlockScheduler();
	}
}


static void
traverse_topology_tree(const cpu_topology_node* node, int packageID, int coreID,
	int32& coreIndex, int32 cpuCount)
{
	switch (node->level) {
		case CPU_TOPOLOGY_SMT:
		{
			bool nodeValid = node->id < cpuCount;
			bool coreValid = coreID < cpuCount;

			if (nodeValid && coreValid) {
				sCPUToCore[node->id] = coreID;
				sCPUToPackage[node->id] = packageID;
				if (sCPUToCluster != NULL)
					sCPUToCluster[node->id] = packageID;
			}
			return;
		}

		case CPU_TOPOLOGY_CORE:
			coreID = coreIndex++;
			break;

		case CPU_TOPOLOGY_PACKAGE:
			packageID = node->id;
			break;

		default:
			break;
	}

	for (int32 i = 0; i < node->children_count; i++) {
		traverse_topology_tree(node->children[i], packageID, coreID, coreIndex,
			cpuCount);
	}
}


static int32
get_topology_id(int32 cpuID)
{
	if (gCPUCacheLevelCount <= 0)
		return sCPUToPackage[cpuID];
	return gCPU[cpuID].cache_id[gCPUCacheLevelCount - 1];
}


static status_t
build_topology_mappings(int32& cpuCount, int32& coreCount, int32& packageCount,
	int32& nodeCount)
{
	cpuCount = smp_get_num_cpus();
	coreCount = 0;
	packageCount = 0;
	nodeCount = 0;

	delete[] sCPUToCore;
	delete[] sCPUToCluster;
	delete[] sPackageToNode;
	delete[] sCPUToPackage;

	sCPUToCore = new(std::nothrow) int32[cpuCount];
	sCPUToCluster = new(std::nothrow) int32[cpuCount];
	sPackageToNode = new(std::nothrow) int32[cpuCount + 1];
	sCPUToPackage = new(std::nothrow) int32[cpuCount];

	if (sCPUToCore == NULL || sCPUToCluster == NULL || sPackageToNode == NULL || sCPUToPackage == NULL) {
		delete[] sCPUToCore;
		delete[] sCPUToCluster;
		delete[] sPackageToNode;
		delete[] sCPUToPackage;
		return B_NO_MEMORY;
	}

	ArrayDeleter<int32> cpuToCoreDeleter(sCPUToCore);
	memset(sCPUToCore, 0, sizeof(int32) * cpuCount);

	ArrayDeleter<int32> cpuToClusterDeleter(sCPUToCluster);
	memset(sCPUToCluster, 0, sizeof(int32) * cpuCount);

	ArrayDeleter<int32> cpuToPackageDeleter(sCPUToPackage);
	memset(sCPUToPackage, 0, sizeof(int32) * cpuCount);

	ArrayDeleter<int32> packageToNodeDeleter(sPackageToNode);
	memset(sPackageToNode, 0, sizeof(int32) * (cpuCount + 1));

	const cpu_topology_node* root = get_cpu_topology();
	int32 coreIndex = 0;

	if (root != NULL)
		traverse_topology_tree(root, 0, 0, coreIndex, cpuCount);
	else {
		for (int32 i = 0; i < cpuCount; i++) {
			sCPUToCore[i] = i;
			sCPUToPackage[i] = 0;
			sCPUToCluster[i] = 0;
		}
	}

	coreCount = 0;
	for (int32 i = 0; i < cpuCount; i++) {
		if (gCPU[i].topology_id[CPU_TOPOLOGY_SMT] == 0)
			coreCount++;
	}

	int32* cpuList = new(std::nothrow) int32[cpuCount];
	if (cpuList == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> cpuListDeleter(cpuList);

	for (int32 i = 0; i < cpuCount; i++)
		cpuList[i] = i;

	bool distinctTopology = false;
	int32 firstTopo = get_topology_id(0);
	for (int32 i = 1; i < cpuCount; i++) {
		if (get_topology_id(i) != firstTopo) {
			distinctTopology = true;
			break;
		}
	}

	TopologyComparator comparator(distinctTopology);

	std::sort(cpuList, cpuList + cpuCount, comparator);

	packageCount = 0;
	nodeCount = 0;

	int32 l3Start = 0;
	while (l3Start < cpuCount) {
		int32 topologyID = distinctTopology ? get_topology_id(cpuList[l3Start]) : (cpuList[l3Start] / 16);
		int32 l3End = l3Start + 1;

		while (l3End < cpuCount) {
			int32 nextTopo = distinctTopology ? get_topology_id(cpuList[l3End]) : (cpuList[l3End] / 16);
			if (nextTopo != topologyID)
				break;
			l3End++;
		}

		int32 coresInL3 = l3End - l3Start;
		int32 targetClusterSize = 4;

		int32 numClusters = (coresInL3 + targetClusterSize / 2)
			/ targetClusterSize;
		if (numClusters < 1)
			numClusters = 1;

		int32 baseSize = coresInL3 / numClusters;
		int32 remainder = coresInL3 % numClusters;

		int32 currentPackageSize = 0;
		int32 clusterIndex = 0;

		int32 currentNodeID = nodeCount++;
		int32 coresInCurrentNode = 0;
		const int32 kMaxCoresPerNode = 16;

		if (coresInL3 > 0) {
			if (packageCount < cpuCount)
				sPackageToNode[packageCount] = currentNodeID;

			for (int32 i = 0; i < coresInL3; i++) {
				int32 cpuID = cpuList[l3Start + i];

				if (coresInCurrentNode >= kMaxCoresPerNode) {
					currentNodeID = nodeCount++;
					coresInCurrentNode = 0;

					if (packageCount < cpuCount)
						sPackageToNode[packageCount] = currentNodeID;
				}

				int32 clusterSize = baseSize + (clusterIndex < remainder ? 1 : 0);
				if (currentPackageSize >= clusterSize) {
					if (packageCount + 1 <= cpuCount) {
						currentPackageSize = 0;
						clusterIndex++;
						packageCount++;
						sPackageToNode[packageCount] = currentNodeID;
					}
				}

				if (packageCount < cpuCount)
					sCPUToCluster[cpuID] = packageCount;

				currentPackageSize++;
				coresInCurrentNode++;
			}

			if (packageCount < cpuCount) {
				packageCount++;
			}
			if (packageCount >= cpuCount) {
				packageCount = cpuCount;
				break;
			}
		}
		l3Start = l3End;
	}

	cpuToCoreDeleter.Detach();
	cpuToPackageDeleter.Detach();
	cpuToClusterDeleter.Detach();
	packageToNodeDeleter.Detach();
	return B_OK;
}


static status_t
init()
{
	gIdleNodeMask = 0;

	gMinCoreType = CORE_TYPE_UNKNOWN;
	gMaxCoreType = CORE_TYPE_UNKNOWN;
	gHasStandardCores = false;

	int32 cpuCount, coreCount, packageCount, nodeCount;
	status_t result = build_topology_mappings(cpuCount, coreCount,
		packageCount, nodeCount);
	if (result != B_OK)
		return result;

	ArrayDeleter<int32> cpuToCoreDeleter(sCPUToCore);
	ArrayDeleter<int32> cpuToPackageDeleter(sCPUToPackage);
	ArrayDeleter<int32> cpuToClusterDeleter(sCPUToCluster);
	ArrayDeleter<int32> packageToNodeDeleter(sPackageToNode);

	if (packageCount > 4096) {
		packageCount = 4096;
	}

	gSingleCore = coreCount == 1;
	scheduler_update_policy();

	gCoreCount = coreCount;
	gPackageCount = packageCount;

	gNodeCount = nodeCount;

	gSchedulerNodes = new(std::nothrow) SchedulerNode[nodeCount];
	if (gSchedulerNodes == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<SchedulerNode> schedulerNodesDeleter(gSchedulerNodes);

	for (int32 i = 0; i < nodeCount; i++)
		gSchedulerNodes[i].Init(i);

	gCPUEntries = new(std::nothrow) CPUEntry[cpuCount];
	gCoreEntries = new(std::nothrow) CoreEntry[coreCount];
	gPackageEntries = new(std::nothrow) PackageEntry[packageCount];

	if (gCPUEntries == NULL || gCoreEntries == NULL || gPackageEntries == NULL) {
		delete[] gCPUEntries;
		delete[] gCoreEntries;
		delete[] gPackageEntries;
		return B_NO_MEMORY;
	}

	ArrayDeleter<CPUEntry> cpuEntriesDeleter(gCPUEntries);
	ArrayDeleter<CoreEntry> coreEntriesDeleter(gCoreEntries);
	ArrayDeleter<PackageEntry> packageEntriesDeleter(gPackageEntries);

	int32 currentNode = -1;
	int32 currentPackageIndexInNode = 0;

	for (int32 i = 0; i < packageCount; i++) {
		int32 nodeIndex = sPackageToNode[i];

		if (nodeIndex != currentNode) {
			if (currentNode != -1) {
				gSchedulerNodes[currentNode].SetPackageCount(
					currentPackageIndexInNode);
			}

			currentNode = nodeIndex;
			currentPackageIndexInNode = gSchedulerNodes[currentNode].PackageCount();
			if (currentPackageIndexInNode == 0)
				gSchedulerNodes[currentNode].SetPackageStartIndex(i);
		}

		int32 packageIndexInNode = currentPackageIndexInNode;
		const int32 kMaxPackagesPerNode = (int32)(sizeof(native_cpu_mask_t) * 8);
		if (packageIndexInNode >= kMaxPackagesPerNode || packageIndexInNode < 0) {
			packageIndexInNode = -1;
		}

		gPackageEntries[i].Init(i, &gSchedulerNodes[nodeIndex],
			packageIndexInNode);

		if (currentPackageIndexInNode != -1)
			currentPackageIndexInNode++;
	}

	if (currentNode != -1) {
		gSchedulerNodes[currentNode].SetPackageCount(
			currentPackageIndexInNode);
	}

	int32* packageCoreCounters = new(std::nothrow) int32[packageCount];
	if (packageCoreCounters == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> packageCoreCountersDeleter(packageCoreCounters);
	memset(packageCoreCounters, 0, sizeof(int32) * packageCount);

	int32* coreToPackage = new(std::nothrow) int32[coreCount];
	if (coreToPackage == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> coreToPackageDeleter(coreToPackage);

	for (int32 i = 0; i < cpuCount; i++)
		coreToPackage[sCPUToCore[i]] = sCPUToCluster[i];

	for (int32 i = 0; i < coreCount; i++) {
		int32 packageID = coreToPackage[i];
		CoreEntry* core = &gCoreEntries[i];

		if (packageID >= packageCount) {
			continue;
		}

		PackageEntry* package = &gPackageEntries[packageID];
		int32 packageIndex = packageCoreCounters[packageID]++;

		if (packageIndex >= kMaxCoresPerPackage) {
			continue;
		}

		core->Init(i, package);
		core->fPackageIndex = packageIndex;
		package->RegisterCore(packageIndex, core);
	}

	for (int32 i = 0; i < cpuCount; i++) {
		CoreEntry* core = &gCoreEntries[sCPUToCore[i]];

		if (core->Package() == NULL) {
			gCPU[i].disabled = true;
			continue;
		}

		gCPUEntries[i].Init(i, core);
		core->AddCPU(&gCPUEntries[i]);
	}

	bool detectedHeterogeneous = false;
	uint64 maxFreq = 0;
	uint64* cpuFreqs = new(std::nothrow) uint64[cpuCount];
	if (cpuFreqs != NULL) {
		ArrayDeleter<uint64> cpuFreqsDeleter(cpuFreqs);
		for (int32 i = 0; i < cpuCount; i++) {
			cpuFreqs[i] = cpu_frequency(i);
			if (cpuFreqs[i] > maxFreq)
				maxFreq = cpuFreqs[i];
		}

		if (maxFreq > 0) {
			bool heterogeneous = false;
			for (int32 i = 0; i < cpuCount; i++) {
				if (cpuFreqs[i] != maxFreq && cpuFreqs[i] != 0) {
					heterogeneous = true;
					break;
				}
			}

			if (heterogeneous) {
				detectedHeterogeneous = true;

				int32 uniqueCapacities[SMP_MAX_CPUS];
				int32 uniqueCapacityCount = 0;

				for (int32 i = 0; i < cpuCount; i++) {
					if (cpuFreqs[i] == 0)
						continue;
					int32 capacity = (cpuFreqs[i] * kDefaultCapacity) / maxFreq;
					if (capacity < 128)
						capacity = 128;

					bool found = false;
					for (int32 j = 0; j < uniqueCapacityCount; j++) {
						if (uniqueCapacities[j] == capacity) {
							found = true;
							break;
						}
					}
					if (!found && uniqueCapacityCount < SMP_MAX_CPUS)
						uniqueCapacities[uniqueCapacityCount++] = capacity;
				}

				for (int32 i = 0; i < uniqueCapacityCount - 1; i++) {
					for (int32 j = 0; j < uniqueCapacityCount - i - 1; j++) {
						if (uniqueCapacities[j] > uniqueCapacities[j + 1]) {
							int32 temp = uniqueCapacities[j];
							uniqueCapacities[j] = uniqueCapacities[j + 1];
							uniqueCapacities[j + 1] = temp;
						}
					}
				}

				for (int32 i = 0; i < cpuCount; i++) {
					int32 coreID = sCPUToCore[i];
					CoreEntry* core = &gCoreEntries[coreID];
					if (cpuFreqs[i] != 0) {
						int32 capacity = (cpuFreqs[i] * kDefaultCapacity)
							/ maxFreq;
						if (capacity < 128)
							capacity = 128;
						core->SetCapacity(capacity);

						for (int32 j = 0; j < uniqueCapacityCount; j++) {
							if (uniqueCapacities[j] == capacity) {
								CoreType type;
								if (uniqueCapacityCount == 1)
									type = CORE_TYPE_STANDARD;
								else if (uniqueCapacityCount == 2)
									type = (j == 0) ? CORE_TYPE_EFFICIENCY : CORE_TYPE_PERFORMANCE;
								else {
									if (j == 0)
										type = CORE_TYPE_EFFICIENCY;
									else if (j == uniqueCapacityCount - 1)
										type = CORE_TYPE_PERFORMANCE;
									else
										type = CORE_TYPE_STANDARD;
								}
								core->SetType(type);
								break;
							}
						}
					}
				}
			}
		}
	}

	if (detectedHeterogeneous && coreCount > 8) {
		bool anyUnknown = false;
		for (int32 i = 0; i < coreCount; i++) {
			if (gCoreEntries[i].Type() == CORE_TYPE_UNKNOWN) {
				anyUnknown = true;
				break;
			}
		}

		if (anyUnknown) {
			int32 eCoreCount = coreCount > 16 ? 8 : coreCount / 2;
			for (int32 i = 0; i < coreCount; i++) {
				if (gCoreEntries[i].Type() != CORE_TYPE_UNKNOWN)
					continue;

				if (i >= coreCount - eCoreCount)
					gCoreEntries[i].SetType(CORE_TYPE_EFFICIENCY);
				else
					gCoreEntries[i].SetType(CORE_TYPE_PERFORMANCE);
			}
		}
	}

	gHasStandardCores = false;
	for (int32 i = 0; i < coreCount; i++) {
		if (gCoreEntries[i].Type() == CORE_TYPE_UNKNOWN)
			gCoreEntries[i].SetType(CORE_TYPE_STANDARD);

		CoreType type = gCoreEntries[i].Type();
		if (type == CORE_TYPE_STANDARD)
			gHasStandardCores = true;

		if (gMinCoreType == CORE_TYPE_UNKNOWN || type < gMinCoreType)
			gMinCoreType = type;
		if (gMaxCoreType == CORE_TYPE_UNKNOWN || type > gMaxCoreType)
			gMaxCoreType = type;
	}

	for (int32 i = 0; i < cpuCount; i++) {
		CPUEntry* cpu = &gCPUEntries[i];
		if (cpu->Core() != NULL)
			cpu->SetPerformanceScale(cpu->Core()->Capacity());
	}

	int32 samples = 16;
	if (packageCount > 16) {
		int32 rootVal = 0;
		while ((rootVal + 1) * (rootVal + 1) <= packageCount)
			rootVal++;
		samples = 16 + rootVal;
	}
	if (samples > 64)
		samples = 64;

	gRandomSamples = samples;

	schedulerNodesDeleter.Detach();
	cpuEntriesDeleter.Detach();
	coreEntriesDeleter.Detach();
	packageEntriesDeleter.Detach();

	sCPUToCore = NULL;
	sCPUToPackage = NULL;
	sCPUToCluster = NULL;
	sPackageToNode = NULL;

	return B_OK;
}


void
scheduler_init()
{
	int32 cpuCount = smp_get_num_cpus();

#ifdef SCHEDULER_PROFILING
	Profiling::Profiler::Initialize();
#endif

	sThreadDataCache = create_object_cache("scheduler thread data",
		sizeof(ThreadData), CACHE_LINE_SIZE, NULL, NULL, NULL);
	if (sThreadDataCache == NULL)
		panic("scheduler_init: failed to create thread data cache");

	status_t result = init();
	if (result != B_OK)
		panic("scheduler_init: failed to initialize scheduler\n");

	scheduler_set_operation_mode(SCHEDULER_MODE_LOW_LATENCY);

	init_debug_commands();

#if SCHEDULER_TRACING
	add_debugger_command_etc("scheduler", &SchedulerTracing::cmd_scheduler,
		"Analyze scheduler tracing information",
		"<thread>\n"
		"Analyzes scheduler tracing information for a given thread.\n"
		"  <thread>  - ID of the thread.\n", 0);
#endif
}


void
scheduler_enable_scheduling()
{
	atomic_set(&sSchedulerEnabled, 1);
}


void
scheduler_update_policy()
{
	gTrackCPULoad = increase_cpu_performance(0) == B_OK;
	gTrackCoreLoad = !gSingleCore || gTrackCPULoad;
}


SchedulerListener::~SchedulerListener()
{
}


void
scheduler_add_listener(struct SchedulerListener* listener)
{
	InterruptsWriteSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Add(listener);
}


void
scheduler_remove_listener(struct SchedulerListener* listener)
{
	InterruptsWriteSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Remove(listener);
}


bigtime_t
_user_estimate_max_scheduling_latency(thread_id id)
{
	syscall_64_bit_return_value();

	Thread* thread;
	if (id < 0) {
		thread = thread_get_current_thread();
		thread->AcquireReference();
	} else {
		thread = Thread::Get(id);
		if (thread == NULL)
			return 0;
	}
	BReference<Thread> threadReference(thread, true);

	InterruptsLocker _;

	ThreadData* threadData = thread->scheduler_data;
	CoreEntry* core = threadData->Core();
	if (core == NULL || core->Package() == NULL) {
		core = CoreEntry::GetCore(smp_get_current_cpu());
	}

	int32 threadCount = core->ThreadCount();
	int32 cpuCount = core->CPUCount();
	if (cpuCount > 0)
		threadCount /= cpuCount;

	if (threadData->GetEffectivePriority() > 0) {
		int32 priority = threadData->GetEffectivePriority();
		if (priority > THREAD_MAX_SET_PRIORITY)
			priority = THREAD_MAX_SET_PRIORITY;

		threadCount -= threadCount * priority / THREAD_MAX_SET_PRIORITY;
	}

	return min_c(max_c(threadCount * Scheduler::BaseQuantum(),
			Scheduler::MinimalQuantum()),
		Scheduler::MaximumLatency());
}


status_t
_user_set_scheduler_mode(int32 mode)
{
	scheduler_mode schedulerMode = static_cast<scheduler_mode>(mode);
	status_t error = scheduler_set_operation_mode(schedulerMode);
	if (error == B_OK)
		cpu_set_scheduler_mode(schedulerMode);
	return error;
}


int32
_user_get_scheduler_mode()
{
	return Scheduler::Mode();
}

void
scheduler_on_team_foreground_changed(Team* team)
{
	SCHEDULER_ENTER_FUNCTION();

	const int kMaxThreadsPerBatch = 256;
	Thread* batch[kMaxThreadsPerBatch];
	bool moreBatches = true;
	Thread* batchStart = NULL;

	BReference<Thread> batchStartRef;

	while (moreBatches) {
		int count = 0;
		moreBatches = false;

		{
			SpinLocker listLocker(team->thread_list_lock);
			Thread* thread = (batchStart == NULL)
				? team->thread_list.First()
				: team->thread_list.GetNext(batchStart);

			while (thread != NULL && count < kMaxThreadsPerBatch) {
				thread->AcquireReference();
				batch[count++] = thread;
				thread = team->thread_list.GetNext(thread);
			}

			if (thread != NULL) {
				batchStart = batch[count - 1];
				batchStartRef.SetTo(batchStart, false);
				moreBatches = true;
			}
		}

		for (int i = 0; i < count; i++) {
			Thread* thread = batch[i];
			BReference<Thread> ref(thread, true);

			InterruptsSpinLocker locker(thread->scheduler_lock);
			ThreadData* threadData = thread->scheduler_data;

			if (threadData == NULL || threadData->IsIdle()
					|| threadData->IsRealTime())
				continue;

			if (thread->state == B_THREAD_READY) {
				if (threadData->Dequeue()) {
					threadData->SetForeground(team->fIsForeground);
					threadData->ResetPriorityBoost();
					enqueue(thread, false, NULL);
				} else {
					threadData->SetForeground(team->fIsForeground);
					threadData->ResetPriorityBoost();
				}
			} else {
				threadData->SetForeground(team->fIsForeground);
				if (thread->state == B_THREAD_RUNNING) {
					threadData->ResetPriorityBoost();
					ASSERT(thread->cpu != NULL);
					CPUEntry* cpu = &gCPUEntries[thread->cpu->cpu_num];
					if (!gCPU[cpu->ID()].disabled) {
						CoreCPUHeapLocker _(threadData->Core());
						cpu->UpdatePriority(
							threadData->GetEffectivePriority());
					}
				}
			}
		}
	}
}

}	// namespace Scheduler
