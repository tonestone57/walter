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


/*! The thread scheduler */


#include <OS.h>

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

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_locking.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"
#include "scheduler_tracing.h"


namespace Scheduler {


class ThreadEnqueuer : public ThreadProcessing {
public:
	void		operator()(ThreadData* thread);
};

scheduler_mode gCurrentModeID;
scheduler_mode_operations* gCurrentMode;

bool gSingleCore;
bool gTrackCoreLoad;
bool gTrackCPULoad;
int32 gRandomSamples;

}	// namespace Scheduler

using namespace Scheduler;


static bool sSchedulerEnabled;

SchedulerListenerList gSchedulerListeners;
rw_spinlock gSchedulerListenersLock = B_RW_SPINLOCK_INITIALIZER;

static scheduler_mode_operations* sSchedulerModes[] = {
	&gSchedulerLowLatencyMode,
	&gSchedulerPowerSavingMode,
};

static object_cache* sThreadDataCache;

// Since CPU IDs used internally by the kernel bear no relation to the actual
// CPU topology the following arrays are used to efficiently get the core
// and the package that CPU in question belongs to.
static int32* sCPUToCore;
static int32* sCPUToPackage;
static int32* sPackageToNode;


static void
UpdatePriorityBoostScalable(CoreEntry* core, CPUEntry* cpu)
{
	SCHEDULER_ENTER_FUNCTION();

	// Scalable Priority Boosting:
	// Instead of scanning all threads (O(N)), we scan only the heads of
	// priority queues (O(1) relative to thread count).
	// We verify if the longest-waiting thread in each queue is starving.
	// This maintains O(1) complexity regardless of the number of threads.

	const int kMaxThreadsToCheckPerQueue = 5;

	// Check CPU RunQueue
	{
		CPURunQueueLocker locker(cpu);
		const ThreadRunQueue* runQueue = cpu->RunQueue();
		const uint32* bitmap = runQueue->GetBitmap();

		for (int i = ThreadRunQueue::kBitmapSize - 1; i >= 0; i--) {
			uint32 val = bitmap[i];
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
					// If thread moved, it was removed from this queue.
					// 'next' is correct either way.
					thread = next;
				}

				val &= ~(1UL << bit);
				if (val == 0)
					break;
				bit = fls(val) - 1;
			}
		}
	}

	// Check Core RunQueue
	{
		CoreRunQueueLocker locker(core);
		const ThreadRunQueue* runQueue = core->RunQueue();
		const uint32* bitmap = runQueue->GetBitmap();

		for (int i = ThreadRunQueue::kBitmapSize - 1; i >= 0; i--) {
			uint32 val = bitmap[i];
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
}


static void enqueue(Thread* thread, bool newOne, Thread* waker);


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


static void
enqueue(Thread* thread, bool newOne, Thread* waker)
{
	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;

	int32 threadPriority = threadData->GetEffectivePriority();
	T(EnqueueThread(thread, threadPriority));

	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;
	if (thread->pinned_to_cpu > 0) {
		ASSERT(thread->previous_cpu != NULL);
		targetCPU = &gCPUEntries[thread->previous_cpu->cpu_num];
		if (gCPU[targetCPU->ID()].disabled)
			targetCPU = NULL;
	} else if (gSingleCore) {
		targetCore = &gCoreEntries[0];
	} else if (waker != NULL && waker->scheduler_data->Core() != NULL) {
		targetCore = waker->scheduler_data->Core();
	} else if (threadData->Core() != NULL
		&& (!newOne || !threadData->HasCacheExpired())) {
		targetCore = threadData->Rebalance();
	}

	const bool rescheduleNeeded = threadData->ChooseCoreAndCPU(targetCore, targetCPU);

	TRACE("enqueueing thread %" B_PRId32 " with priority %" B_PRId32 " on CPU %" B_PRId32 " (core %" B_PRId32 ")\n",
		thread->id, threadPriority, targetCPU->ID(), targetCore->ID());

	bool wasRunQueueEmpty = false;
	bool requestPreemption = false;
	threadData->Enqueue(wasRunQueueEmpty, requestPreemption);

	// notify listeners
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
			smp_send_ici(targetCPU->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0,
				NULL, SMP_MSG_FLAG_ASYNC);
		}
	}
}


/*!	Enqueues the thread into the run queue.
	Note: thread lock must be held when entering this function
*/
void
scheduler_enqueue_in_run_queue(Thread *thread)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	SchedulerModeLocker _;

	TRACE("enqueueing new thread %" B_PRId32 " with static priority %" B_PRId32 "\n", thread->id,
		thread->priority);

	ThreadData* threadData = thread->scheduler_data;
	Thread* waker = thread->waker;
	thread->waker = NULL;

	threadData->ResetPriorityBoost();
	enqueue(thread, true, waker);
}


/*!	Sets the priority of a thread.
*/
int32
scheduler_set_thread_priority(Thread *thread, int32 priority)
{
	ASSERT(are_interrupts_enabled());

	InterruptsSpinLocker _(thread->scheduler_lock);
	SchedulerModeLocker modeLocker;

	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;
	int32 oldPriority = thread->priority;

	TRACE("changing thread %" B_PRId32 " priority to %" B_PRId32 " (old: %" B_PRId32 ", effective: %" B_PRId32 ")\n",
		thread->id, priority, oldPriority, threadData->GetEffectivePriority());

	thread->priority = priority;
	threadData->ResetPriorityBoost();

	if (priority == oldPriority)
		return oldPriority;

	if (thread->state != B_THREAD_READY) {
		if (thread->state == B_THREAD_RUNNING) {
			ASSERT(threadData->Core() != NULL);

			ASSERT(thread->cpu != NULL);
			CPUEntry* cpu = &gCPUEntries[thread->cpu->cpu_num];

			CoreCPUHeapLocker _(threadData->Core());
			cpu->UpdatePriority(priority);
		}

		return oldPriority;
	}

	// The thread is in the run queue. We need to remove it and re-insert it at
	// a new position.

	T(RemoveThread(thread));

	// notify listeners
	NotifySchedulerListeners(&SchedulerListener::ThreadRemovedFromRunQueue,
		thread);

	if (threadData->Dequeue())
		enqueue(thread, true, NULL);

	return oldPriority;
}


void
scheduler_reschedule_ici()
{
	// This function is called as a result of an incoming ICI.
	// Make sure the reschedule() is invoked.
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

	release_spinlock(&cpu->previous_thread->scheduler_lock);

	// continue CPU time based user timers
	continue_cpu_timers(thread, cpu);

	// notify the user debugger code
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


/*!	Switches the currently running thread.
	This is a service function for scheduler implementations.

	\param fromThread The currently running thread.
	\param toThread The thread to switch to. Must be different from
		\a fromThread.
*/
static inline void
switch_thread(Thread* fromThread, Thread* toThread)
{
	// notify the user debugger code
	if ((fromThread->flags & THREAD_FLAGS_DEBUGGER_INSTALLED) != 0)
		user_debug_thread_unscheduled(fromThread);

	// stop CPU time based user timers
	stop_cpu_timers(fromThread, toThread);

	// update CPU and Thread structures and perform the context switch
	cpu_ent* cpu = fromThread->cpu;
	toThread->previous_cpu = toThread->cpu = cpu;
	fromThread->cpu = NULL;
	cpu->running_thread = toThread;
	cpu->previous_thread = fromThread;

	arch_thread_set_current_thread(toThread);
	arch_thread_context_switch(fromThread, toThread);

	// The use of fromThread below looks weird, but is correct. fromThread had
	// been unscheduled earlier, but is back now. For a thread scheduled the
	// first time the same is done in thread.cpp:common_thread_entry().
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
	CoreEntry* core = CoreEntry::GetCore(thisCPU);

	Thread* oldThread = thread_get_current_thread();
	ThreadData* oldThreadData = oldThread->scheduler_data;

	CPUSet oldThreadMask;
	bool useOldThreadMask, fetchedOldThreadMask = false;

	oldThreadData->StopCPUTime();

	SchedulerModeLocker modeLocker;

	TRACE("reschedule(): cpu %" B_PRId32 ", current thread = %" B_PRId32 "\n", thisCPU,
		oldThread->id);

	oldThread->state = nextState;

	// return time spent in interrupts
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
					TRACE("enqueueing thread %ld into run queue priority ="
						" %ld\n", oldThread->id,
						oldThreadData->GetEffectivePriority());
					putOldThreadAtBack = true;
				} else {
					TRACE("putting thread %ld back in run queue priority ="
						" %ld\n", oldThread->id,
						oldThreadData->GetEffectivePriority());
					putOldThreadAtBack = false;
				}
			}

			break;
		case THREAD_STATE_FREE_ON_RESCHED:
			oldThreadData->Dies();
			break;
		default:
			oldThreadData->GoesAway();
			TRACE("not enqueueing thread %ld into run queue next_state = %ld\n",
				oldThread->id, nextState);
			break;
	}

	oldThread->has_yielded = false;

	// select thread with the biggest priority and enqueue back the old thread
	ThreadData* nextThreadData;
	if (gCPU[thisCPU].disabled) {
		if (!oldThreadData->IsIdle()) {
			putOldThreadAtBack = true;
			oldThreadData->UnassignCore(true);

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

		if (oldThreadShouldMigrate) {
			enqueue(oldThread, true, NULL);
			// replace with the idle thread, if no other thread could be found
			if (oldThreadData == nextThreadData)
				nextThreadData = cpu->PeekIdleThread();
		}

		// update CPU heap
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

	TRACE("reschedule(): cpu %" B_PRId32 ", next thread = %" B_PRId32 "\n", thisCPU,
		nextThread->id);

	T(ScheduleThread(nextThread, oldThread));

	// notify listeners
	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled,
		oldThread, nextThread);

	ASSERT(nextThreadData->Core() == core);
	nextThread->state = B_THREAD_RUNNING;
	nextThreadData->StartCPUTime();

	// track CPU activity
	cpu->TrackActivity(oldThreadData, nextThreadData);

	if (nextThread != oldThread || oldThread->cpu->preempted) {
		cpu->StartQuantumTimer(nextThreadData, oldThread->cpu->preempted);

		oldThread->cpu->preempted = false;
		if (!nextThreadData->IsIdle())
			nextThreadData->Continues();
		else
			gCurrentMode->rebalance_irqs(true);
		nextThreadData->StartQuantum();

		modeLocker.Unlock();

		SCHEDULER_EXIT_FUNCTION();

		if (nextThread != oldThread)
			switch_thread(oldThread, nextThread);
	}
}


/*!	Runs the scheduler.
	Note: expects thread spinlock to be held
*/
void
scheduler_reschedule(int32 nextState)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	if (!sSchedulerEnabled) {
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


/*!	This starts the scheduler. Must be run in the context of the initial idle
	thread. Interrupts must be disabled and will be disabled when returning.
*/
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

	dprintf("scheduler: switching to %s mode\n", sSchedulerModes[mode]->name);

	InterruptsBigSchedulerLocker _;

	gCurrentModeID = mode;
	gCurrentMode = sSchedulerModes[mode];
	gCurrentMode->switch_to_mode();

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

	dprintf("scheduler: %s CPU %" B_PRId32 "\n",
		enabled ? "enabling" : "disabling", cpuID);

	InterruptsBigSchedulerLocker _;

	gCurrentMode->set_cpu_enabled(cpuID, enabled);

	CPUEntry* cpu = &gCPUEntries[cpuID];
	CoreEntry* core = cpu->Core();

	ASSERT(core->CPUCount() >= 0);

	if (enabled) {
		cpu->Start();
		gCPU[cpuID].disabled = false;
		gCPUEnabled.SetBitAtomic(cpuID);
	} else {
		gCPU[cpuID].disabled = true;
		gCPUEnabled.ClearBitAtomic(cpuID);

		ThreadEnqueuer enqueuer;

		// flush CPU run queue
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

		cpu->UpdatePriority(B_IDLE_PRIORITY);
		core->RemoveCPU(cpu, enqueuer);

		cpu->Stop();

		// don't wait until the thread quantum ends
		if (smp_get_current_cpu() != cpuID) {
			smp_send_ici(cpuID, SMP_MSG_RESCHEDULE, 0, 0, 0, NULL,
				SMP_MSG_FLAG_ASYNC);
		}
	}
}


static void
traverse_topology_tree(const cpu_topology_node* node, int packageID, int coreID)
{
	switch (node->level) {
		case CPU_TOPOLOGY_SMT:
			sCPUToCore[node->id] = coreID;
			sCPUToPackage[node->id] = packageID;
			return;

		case CPU_TOPOLOGY_CORE:
			coreID = node->id;
			break;

		case CPU_TOPOLOGY_PACKAGE:
			packageID = node->id;
			break;

		default:
			break;
	}

	for (int32 i = 0; i < node->children_count; i++)
		traverse_topology_tree(node->children[i], packageID, coreID);
}


static int32
get_topology_id(int32 cpuID)
{
	if (gCPUCacheLevelCount > 0)
		return gCPU[cpuID].cache_id[gCPUCacheLevelCount - 1];
	return sCPUToPackage[cpuID];
}


static status_t
build_topology_mappings(int32& cpuCount, int32& coreCount, int32& packageCount,
	int32& nodeCount)
{
	cpuCount = smp_get_num_cpus();

	sCPUToCore = new(std::nothrow) int32[cpuCount];
	if (sCPUToCore == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> cpuToCoreDeleter(sCPUToCore);

	sCPUToPackage = new(std::nothrow) int32[cpuCount];
	if (sCPUToPackage == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> cpuToPackageDeleter(sCPUToPackage);

	// Safe upper bound allocation for mapping packages to nodes
	sPackageToNode = new(std::nothrow) int32[cpuCount];
	if (sPackageToNode == NULL)
		return B_NO_MEMORY;
	// No deleter for sPackageToNode as it persists like sCPUToPackage

	// First pass: logical topology from ACPI/Device Tree
	const cpu_topology_node* root = get_cpu_topology();
	traverse_topology_tree(root, 0, 0);

	coreCount = 0;
	for (int32 i = 0; i < cpuCount; i++) {
		if (gCPU[i].topology_id[CPU_TOPOLOGY_SMT] == 0)
			coreCount++;
	}

	// Second pass: Topology-Aware Clustering
	// Cluster Strategy:
	// 1. Group CPUs by L3 Cache ID (Topology Domain).
	// 2. Map each L3 domain to a unique SchedulerNode.
	// 3. Within each L3 domain, split cores into Packages (Clusters) of target size 4.
	// 4. Balance "runt" clusters (5-7 cores) evenly (e.g., 6 -> 3+3, not 4+2).

	int32* cpuList = new(std::nothrow) int32[cpuCount];
	if (cpuList == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> cpuListDeleter(cpuList);

	for (int32 i = 0; i < cpuCount; i++)
		cpuList[i] = i;

	// Sort by L3 Topology ID
	for (int32 i = 1; i < cpuCount; i++) {
		int32 key = cpuList[i];
		int32 topoKey = get_topology_id(key);
		int32 j = i - 1;

		while (j >= 0) {
			int32 compare = cpuList[j];
			int32 compareTopo = get_topology_id(compare);

			if (compareTopo > topoKey
				|| (compareTopo == topoKey && compare > key)) {
				cpuList[j + 1] = cpuList[j];
				j--;
			} else {
				break;
			}
		}
		cpuList[j + 1] = key;
	}

	packageCount = 0;
	nodeCount = 0;

	int32 l3Start = 0;
	while (l3Start < cpuCount) {
		int32 topologyID = get_topology_id(cpuList[l3Start]);
		int32 l3End = l3Start + 1;

		// Find end of current L3 domain
		while (l3End < cpuCount && get_topology_id(cpuList[l3End]) == topologyID)
			l3End++;

		int32 coresInL3 = l3End - l3Start;
		int32 targetClusterSize = 4;

		// Calculate balanced cluster sizes
		// Formula: (N + target - 1) / target gives minimal number of clusters
		// to satisfy max size <= target.
		// However, we want to balance.
		// Example: 6 cores. (6+3)/4 = 2 clusters. 6/2 = 3 cores each.
		int32 numClusters = (coresInL3 + targetClusterSize - 1) / targetClusterSize;
		int32 baseSize = coresInL3 / numClusters;
		int32 remainder = coresInL3 % numClusters;

		int32 currentPackageSize = 0;
		int32 assignedInL3 = 0;
		int32 clusterIndex = 0;

		// Create a SchedulerNode for this L3 domain
		int32 currentNodeID = nodeCount++;

		for (int32 i = 0; i < coresInL3; i++) {
			int32 cpuID = cpuList[l3Start + i];
			int32 clusterSize = baseSize + (clusterIndex < remainder ? 1 : 0);

			if (currentPackageSize >= clusterSize) {
				packageCount++;
				currentPackageSize = 0;
				clusterIndex++;
			}

			sCPUToPackage[cpuID] = packageCount;
			sPackageToNode[packageCount] = currentNodeID;
			currentPackageSize++;
		}
		packageCount++; // Finish last package in L3
		l3Start = l3End;
	}

	cpuToCoreDeleter.Detach();
	cpuToPackageDeleter.Detach();
	return B_OK;
}


static status_t
init()
{
	// create logical processor to core and package mappings
	int32 cpuCount, coreCount, packageCount, nodeCount;
	status_t result = build_topology_mappings(cpuCount, coreCount,
		packageCount, nodeCount);
	if (result != B_OK)
		return result;

	if (packageCount > 4096) {
		dprintf("scheduler: system has too many packages (%" B_PRId32 " > 4096). "
			"Limiting to 4096 packages. Excess cores will be disabled.\n",
			packageCount);
		packageCount = 4096;
	}

	// disable parts of the scheduler logic that are not needed
	gSingleCore = coreCount == 1;
	scheduler_update_policy();

	gCoreCount = coreCount;
	gPackageCount = packageCount;

	// Use topology-aware nodes detected by build_topology_mappings
	if (nodeCount > 64) {
		dprintf("scheduler: limiting nodes to 64 (was %" B_PRId32 ")\n", nodeCount);
		nodeCount = 64;
	}
	gNodeCount = nodeCount;

	gSchedulerNodes = new(std::nothrow) SchedulerNode[nodeCount];
	if (gSchedulerNodes == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<SchedulerNode> schedulerNodesDeleter(gSchedulerNodes);

	for (int32 i = 0; i < nodeCount; i++)
		gSchedulerNodes[i].Init(i);

	gCPUEntries = new(std::nothrow) CPUEntry[cpuCount];
	if (gCPUEntries == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<CPUEntry> cpuEntriesDeleter(gCPUEntries);

	gCoreEntries = new(std::nothrow) CoreEntry[coreCount];
	if (gCoreEntries == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<CoreEntry> coreEntriesDeleter(gCoreEntries);

	gPackageEntries = new(std::nothrow) PackageEntry[packageCount];
	if (gPackageEntries == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<PackageEntry> packageEntriesDeleter(gPackageEntries);

	for (int32 i = 0; i < packageCount; i++) {
		int32 nodeIndex = sPackageToNode[i];
		if (nodeIndex >= nodeCount)
			nodeIndex = 0; // Fallback for edge cases
		gPackageEntries[i].Init(i, &gSchedulerNodes[nodeIndex]);
	}

	// Map Core to Package and assign index within package
	int32* packageCoreCounters = new(std::nothrow) int32[packageCount];
	if (packageCoreCounters == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> packageCoreCountersDeleter(packageCoreCounters);
	memset(packageCoreCounters, 0, sizeof(int32) * packageCount);

	// Determine package index for each core
	// We need to iterate cores, but we only have map CPU->Core and CPU->Package
	int32* coreToPackage = new(std::nothrow) int32[coreCount];
	if (coreToPackage == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> coreToPackageDeleter(coreToPackage);

	for (int32 i = 0; i < cpuCount; i++)
		coreToPackage[sCPUToCore[i]] = sCPUToPackage[i];

	for (int32 i = 0; i < coreCount; i++) {
		int32 packageID = coreToPackage[i];
		CoreEntry* core = &gCoreEntries[i];

		if (packageID >= packageCount) {
			// This core belongs to a package beyond the limit. Skip initialization.
			continue;
		}

		PackageEntry* package = &gPackageEntries[packageID];
		int32 packageIndex = packageCoreCounters[packageID]++;

		if (packageIndex >= kMaxCoresPerPackage) {
			// Disable excess cores instead of panicking
			dprintf("Scheduler: Package %" B_PRId32 " has too many cores (%" B_PRId32
				" > %" B_PRId32 "). Disabling core %" B_PRId32 ".\n",
				packageID, packageIndex + 1, kMaxCoresPerPackage, i);

			// We can't easily mark it disabled here as we are iterating cores, not CPUs.
			// But we skip Init(), so Package() remains NULL.
			// The next loop iterates CPUs and checks if Core->Package() is NULL.
			continue;
		}

		core->Init(i, package);
		core->fPackageIndex = packageIndex;
		package->RegisterCore(packageIndex, core);
	}

	for (int32 i = 0; i < cpuCount; i++) {
		CoreEntry* core = &gCoreEntries[sCPUToCore[i]];

		if (core->Package() == NULL) {
			dprintf("scheduler: disabling cpu %" B_PRId32 " (topology limit)\n", i);
			gCPU[i].disabled = true;
			continue;
		}

		gCPUEntries[i].Init(i, core);
		core->AddCPU(&gCPUEntries[i]);
	}

	// Determine CPU Capacities (Heterogeneous Support)
	// We use the CPU frequency as a proxy for performance capacity.
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
				dprintf("scheduler: heterogeneous CPUs detected (max frequency: %"
					B_PRIu64 ")\n", maxFreq);
				for (int32 i = 0; i < cpuCount; i++) {
					int32 coreID = sCPUToCore[i];
					CoreEntry* core = &gCoreEntries[coreID];
					if (cpuFreqs[i] != 0) {
						int32 capacity = (cpuFreqs[i] * kDefaultCapacity)
							/ maxFreq;
						// Enforce a minimum capacity to avoid division by zero anomalies
						if (capacity < 128)
							capacity = 128;
						core->SetCapacity(capacity);
					}
				}
			}
		}
	}

	// Calculate dynamic random sampling parameters based on system size
	// We aim for sqrt(N) scaling to maintain coverage without linear cost.
	// Baseline: 16 samples for small/medium systems (up to ~256 packages)
	// Max: 64 samples for massive systems (4096 packages)
	// Simple heuristic: 16 + sqrt(packageCount)
	int32 samples = 16;
	if (packageCount > 16) {
		// Integer square root approximation
		int32 root = 0;
		while ((root + 1) * (root + 1) <= packageCount)
			root++;
		samples = 16 + root;
	}
	// Clamp to a reasonable maximum to ensure O(1) bound
	if (samples > 64)
		samples = 64;

	gRandomSamples = samples;
	dprintf("scheduler: dynamic random sampling set to %" B_PRId32 " (packages: %" B_PRId32 ")\n",
		gRandomSamples, packageCount);

	packageEntriesDeleter.Detach();
	coreEntriesDeleter.Detach();
	cpuEntriesDeleter.Detach();
	schedulerNodesDeleter.Detach();

	return B_OK;
}


void
scheduler_init()
{
	int32 cpuCount = smp_get_num_cpus();
	dprintf("scheduler_init: found %" B_PRId32 " logical cpu%s and %" B_PRId32
		" cache level%s\n", cpuCount, cpuCount != 1 ? "s" : "",
		gCPUCacheLevelCount, gCPUCacheLevelCount != 1 ? "s" : "");

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
	add_debugger_command_etc("scheduler", &cmd_scheduler,
		"Analyze scheduler tracing information",
		"<thread>\n"
		"Analyzes scheduler tracing information for a given thread.\n"
		"  <thread>  - ID of the thread.\n", 0);
#endif
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


// #pragma mark - SchedulerListener


SchedulerListener::~SchedulerListener()
{
}


// #pragma mark - kernel private


/*!	Add the given scheduler listener. Thread lock must be held.
*/
void
scheduler_add_listener(struct SchedulerListener* listener)
{
	InterruptsWriteSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Add(listener);
}


/*!	Remove the given scheduler listener. Thread lock must be held.
*/
void
scheduler_remove_listener(struct SchedulerListener* listener)
{
	InterruptsWriteSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Remove(listener);
}


// #pragma mark - Syscalls


bigtime_t
_user_estimate_max_scheduling_latency(thread_id id)
{
	syscall_64_bit_return_value();

	// get the thread
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

#ifdef SCHEDULER_PROFILING
	InterruptsLocker _;
#endif

	ThreadData* threadData = thread->scheduler_data;
	CoreEntry* core = threadData->Core();
	if (core == NULL) {
		// Pick a random active core
		// Loop until we find an initialized core (one with a package assigned)
		// We use a retry limit to avoid infinite loops in degenerate cases
		const int kMaxCoreSelectionRetries = 100;
		int retries = 0;
		do {
			core = &gCoreEntries[fast_get_random<uint32>() % gCoreCount];
		} while (core->Package() == NULL && retries++ < kMaxCoreSelectionRetries);

		// Fallback to the first core if random selection failed
		if (core->Package() == NULL)
			core = &gCoreEntries[0];
	}

	int32 threadCount = core->ThreadCount();
	if (core->CPUCount() > 0)
		threadCount /= core->CPUCount();

	if (threadData->GetEffectivePriority() > 0) {
		threadCount -= threadCount * THREAD_MAX_SET_PRIORITY
				/ threadData->GetEffectivePriority();
	}

	return min_c(max_c(threadCount * gCurrentMode->base_quantum,
			gCurrentMode->minimal_quantum),
		gCurrentMode->maximum_latency);
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
	return gCurrentModeID;
}
