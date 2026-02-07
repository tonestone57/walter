/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */


#include "scheduler_cpu.h"

#include <new>

#include <util/AutoLock.h>
#include <util/Random.h>

#include "scheduler_thread.h"
#include "scheduler_topology.h"


namespace Scheduler {


CPUEntry* gCPUEntries;

CoreEntry* gCoreEntries;
int32 gCoreCount;

PackageEntry* gPackageEntries;
int32 gPackageCount;

SchedulerNode* gSchedulerNodes;
uint64 gIdleNodeMask = 0;
int32 gNodeCount;


}	// namespace Scheduler

using namespace Scheduler;


class Scheduler::DebugDumper {
public:
	static	void		DumpCPURunQueue(CPUEntry* cpu);
	static	void		DumpCoreRunQueue(CoreEntry* core);
	static	void		DumpCoreEntryLoad(CoreEntry* core);
	static	void		DumpIdleCoresInPackage(PackageEntry* package);
	static	void		DumpPackageCores(PackageEntry* package);

private:
	struct CoreThreadsData {
			CoreEntry*	fCore;
			int32		fLoad;
	};

	static	void		_AnalyzeCoreThreads(Thread* thread, void* data);
};


static CPUPriorityHeap sDebugCPUHeap;


void
ThreadRunQueue::Dump() const
{
	ThreadRunQueue::ConstIterator iterator = GetConstIterator();
	if (!iterator.HasNext())
		kprintf("Run queue is empty.\n");
	else {
		kprintf("thread      id      priority penalty  name\n");
		while (iterator.HasNext()) {
			ThreadData* threadData = iterator.Next();
			Thread* thread = threadData->GetThread();

			kprintf("%p  %-7" B_PRId32 " %-8" B_PRId32 " %-8" B_PRId32 " %s\n",
				thread, thread->id, thread->priority,
				thread->priority - threadData->GetEffectivePriority(),
				thread->name);
		}
	}
}


CPUEntry::CPUEntry()
	:
	fLoad(0),
	fMeasureActiveTime(0),
	fMeasureTime(0),
	fUpdateLoadEvent(false),
	fRandomState(0x12345678)
{
	B_INITIALIZE_RW_SPINLOCK(&fSchedulerModeLock);
	B_INITIALIZE_SPINLOCK(&fQueueLock);
}


void
CPUEntry::Init(int32 id, CoreEntry* core)
{
	fCPUNumber = id;
	fCore = core;
	fRandomState = (uint32)system_time() + id * 31337 + 1;
	if (fRandomState == 0)
		fRandomState = 0x12345678;
}


void
CPUEntry::Start()
{
	fLoad = 0;
	fCore->AddCPU(this);
}


void
CPUEntry::Stop()
{
	cpu_ent* entry = &gCPU[fCPUNumber];

	// get rid of irqs
	SpinLocker locker(entry->irqs_lock);
	while (true) {
		irq_assignment* irq
			= (irq_assignment*)list_get_first_item(&entry->irqs);
		if (irq == NULL)
			break;

		int32 irqVector = irq->irq;
		locker.Unlock();

		assign_io_interrupt_to_cpu(irqVector, -1);

		locker.Lock();

		irq_assignment* currentHead
			= (irq_assignment*)list_get_first_item(&entry->irqs);
		if (currentHead != NULL && currentHead->irq == irqVector)
			break;
	}
	locker.Unlock();
}


void
CPUEntry::PushFront(ThreadData* thread, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();
	fRunQueue.PushFront(thread, priority);
}


void
CPUEntry::PushBack(ThreadData* thread, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();
	fRunQueue.PushBack(thread, priority);
}


void
CPUEntry::Remove(ThreadData* thread)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(thread->IsEnqueued());
	thread->SetDequeued();
	fRunQueue.Remove(thread);
}


struct ThreadDataVRuntimeCompare {
	bool operator()(const ThreadData* a, const ThreadData* b) const
	{
		if (a->IsRealTime())
			return false;
		return a->GetVirtualRuntime() < b->GetVirtualRuntime();
	}
};


struct ThreadDataOptimal {
	bool operator()(const ThreadData* thread) const
	{
		if (thread->IsRealTime())
			return true;

		// The logic below was intended to be an early exit optimization:
		// "If a thread is within 2ms of the minimum virtual runtime, it is fair enough."
		// However, since fMinVRuntime is derived from the head of the queue (maximum priority,
		// and often minimum virtual runtime due to insertion order), this check almost always
		// returns true for the first candidate, effectively disabling the search for a better
		// candidate (Dynamic Search Depth).
		//
		// To ensure we actually scan the run queue (up to kSearchDepth in RunQueue::PeekBest)
		// to find the thread with the strictly lowest virtual runtime, we must return false here.
		//
		// Note: This tradeoff prioritizes fairness (strictly lowest virtual runtime) over
		// absolute minimum scheduling latency (picking the first available).
		return false;
	}
};


ThreadData*
CoreEntry::PeekThread() const
{
	SCHEDULER_ENTER_FUNCTION();
	return fRunQueue.PeekBest(ThreadDataVRuntimeCompare(),
		ThreadDataOptimal());
}


ThreadData*
CPUEntry::PeekThread() const
{
	SCHEDULER_ENTER_FUNCTION();
	return fRunQueue.PeekBest(ThreadDataVRuntimeCompare(),
		ThreadDataOptimal());
}


ThreadData*
CPUEntry::PeekIdleThread() const
{
	SCHEDULER_ENTER_FUNCTION();
	return fRunQueue.GetHead(B_IDLE_PRIORITY);
}


void
CPUEntry::UpdatePriority(int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!gCPU[fCPUNumber].disabled || priority == B_IDLE_PRIORITY);

	int32 oldPriority = CPUPriorityHeap::GetKey(this);
	if (oldPriority == priority)
		return;
	fCore->CPUHeap()->ModifyKey(this, priority);

	if (oldPriority == B_IDLE_PRIORITY)
		fCore->CPUWakesUp(this);
	else if (priority == B_IDLE_PRIORITY)
		fCore->CPUGoesIdle(this);
}


void
CPUEntry::ComputeLoad()
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCPULoad);
	ASSERT(!gCPU[fCPUNumber].disabled);
	ASSERT(fCPUNumber == smp_get_current_cpu());

	int oldLoad = compute_load(fMeasureTime, fMeasureActiveTime, fLoad,
			system_time());
	if (oldLoad < 0)
		return;

	if (fLoad > kVeryHighLoad)
		gCurrentMode->rebalance_irqs(false);
}


ThreadData*
CPUEntry::ChooseNextThread(ThreadData* oldThread, bool putAtBack)
{
	SCHEDULER_ENTER_FUNCTION();

	int32 oldPriority = -1;
	if (oldThread != NULL)
		oldPriority = oldThread->GetEffectivePriority();

	CPURunQueueLocker cpuLocker(this);

	ThreadData* pinnedThread = fRunQueue.PeekMaximum();
	int32 pinnedPriority = -1;
	if (pinnedThread != NULL)
		pinnedPriority = pinnedThread->GetEffectivePriority();

	CoreRunQueueLocker coreLocker(fCore);

	ThreadData* sharedThread = fCore->PeekThread();
	if (sharedThread == NULL && pinnedThread == NULL) {
		// try to steal work from other cores in the same package
		sharedThread = _TryStealWork();
	}

	bool sharedThreadIsFloating = sharedThread != NULL
		&& !sharedThread->IsEnqueued();

	if (sharedThread == NULL && pinnedThread == NULL && oldThread == NULL)
		return NULL;

	int32 sharedPriority = -1;
	if (sharedThread != NULL)
		sharedPriority = sharedThread->GetEffectivePriority();

	int32 rest = max_c(pinnedPriority, sharedPriority);
	if (oldPriority > rest || (!putAtBack && oldPriority == rest)) {
		if (sharedThreadIsFloating) {
			coreLocker.Unlock();
			bool wasRunQueueEmpty;
			bool requestPreemption;
			sharedThread->Enqueue(wasRunQueueEmpty, requestPreemption);
		}
		return oldThread;
	}

	if (sharedPriority > pinnedPriority) {
		if (sharedThread->Core() == fCore && !sharedThreadIsFloating)
			fCore->Remove(sharedThread);
		return sharedThread;
	}

	coreLocker.Unlock();

	if (sharedThreadIsFloating) {
		bool wasRunQueueEmpty;
		bool requestPreemption;
		sharedThread->Enqueue(wasRunQueueEmpty, requestPreemption);
	}

	Remove(pinnedThread);
	return pinnedThread;
}


void
CPUEntry::UpdateActiveTime(ThreadData* oldThreadData)
{
	SCHEDULER_ENTER_FUNCTION();

	cpu_ent* cpuEntry = &gCPU[fCPUNumber];

	Thread* oldThread = oldThreadData->GetThread();
	if (!thread_is_idle_thread(oldThread)) {
		bigtime_t active
			= (oldThread->kernel_time - cpuEntry->last_kernel_time)
				+ (oldThread->user_time - cpuEntry->last_user_time);

		WriteSequentialLocker locker(cpuEntry->active_time_lock);
		cpuEntry->active_time += active;
		locker.Unlock();

		fMeasureActiveTime += active;
		fCore->IncreaseActiveTime(active);

		oldThreadData->UpdateActivity(active);
	}
}


void
CPUEntry::TrackLoad(ThreadData* nextThreadData)
{
	SCHEDULER_ENTER_FUNCTION();

	cpu_ent* cpuEntry = &gCPU[fCPUNumber];

	if (gTrackCPULoad) {
		if (!cpuEntry->disabled)
			ComputeLoad();
		_RequestPerformanceLevel(nextThreadData);
	}

	Thread* nextThread = nextThreadData->GetThread();
	if (!thread_is_idle_thread(nextThread)) {
		cpuEntry->last_kernel_time = nextThread->kernel_time;
		cpuEntry->last_user_time = nextThread->user_time;

		nextThreadData->SetLastInterruptTime(cpuEntry->interrupt_time);
	}
}


uint32
CPUEntry::GetRandom()
{
	uint32 x = fRandomState;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	fRandomState = x;
	return x;
}


ThreadData*
CPUEntry::_TryStealWork()
{
	SCHEDULER_ENTER_FUNCTION();

	// iterate over other cores in the package and try to steal work
	PackageEntry* package = fCore->Package();

	int32 registeredCores = package->RegisteredCoreCount();
	if (registeredCores <= 1)
		return NULL;

	// Pick a random starting point to avoid convoys
	// We use multiplicative mapping to avoid modulo.
	int32 startIndex = (int32)(((uint64)GetRandom() * registeredCores) >> 32);

	for (int32 i = 0; i < registeredCores; i++) {
		// Optimization: Use subtraction for wrapping instead of modulo
		int32 index = startIndex + i;
		if (index >= registeredCores)
			index -= registeredCores;

		CoreEntry* victim = package->GetCore(index);

		if (victim == NULL || victim == fCore || victim->CPUCount() == 0)
			continue;

		// Use TryLock to avoid contention
		if (victim->TryLockRunQueue()) {
			int32 stolenPriority = -1;
			ThreadData* stolen = victim->StealThread(stolenPriority, fCPUNumber);

			if (stolen != NULL)
				stolen->MigrateTo(fCore);

			victim->UnlockRunQueue();

			if (stolen != NULL)
				return stolen;
		}
	}

	// Phase 2: The Local NUMA Node (Random)
	// Target: Cores on the same physical socket/die (e.g., 64-128 cores).
	// Method: Fixed Random (e.g., 4-8 probes).
	// Why: Stealing here is fast (local RAM). You want to exhaust reasonable options here
	// before going across the expensive interconnect.

	SchedulerNode* node = package->Node();
	ThreadData* stolen = NULL;

	search_local_node(node, [&](PackageEntry* entry) {
		if (stolen != NULL)
			return true;

		if (entry == package)
			return false;

		int32 victimCoreCount = entry->RegisteredCoreCount();
		if (victimCoreCount == 0)
			return false;

		int32 coreIndex = (int32)(((uint64)GetRandom() * victimCoreCount) >> 32);
		CoreEntry* victim = entry->GetCore(coreIndex);

		if (victim == NULL)
			return false;

		if ((entry->IdleCoreMask() & (1U << victim->PackageIndex())) != 0)
			return false;

		if (victim->TryLockRunQueue()) {
			int32 stolenPriority = -1;
			stolen = victim->StealThread(stolenPriority, fCPUNumber);

			if (stolen != NULL)
				stolen->MigrateTo(fCore);

			victim->UnlockRunQueue();
		}

		return stolen != NULL;
	});

	if (stolen != NULL)
		return stolen;

	// Phase 3: The Global Hail Mary (Random)
	// Target: Any core in the system (4096 cores).
	// Method: Logarithmic Formula
	// Why: This is the last resort. If the local node is empty, you are willing to pay
	// the high cost to steal from a remote socket to avoid sleeping.

	search_global_random([&](PackageEntry* entry) {
		if (stolen != NULL)
			return true;

		if (entry == package)
			return false;

		if (entry->IdleCoreCount() == entry->CoreCount())
			return false;

		int32 victimCoreCount = entry->RegisteredCoreCount();
		if (victimCoreCount == 0)
			return false;

		int32 coreIndex = (int32)(((uint64)GetRandom() * victimCoreCount) >> 32);
		CoreEntry* victim = entry->GetCore(coreIndex);

		if (victim == NULL)
			return false;

		if ((entry->IdleCoreMask() & (1U << victim->PackageIndex())) != 0)
			return false;

		if (victim->TryLockRunQueue()) {
			int32 stolenPriority = -1;
			stolen = victim->StealThread(stolenPriority, fCPUNumber);

			if (stolen != NULL)
				stolen->MigrateTo(fCore);

			victim->UnlockRunQueue();
		}

		return stolen != NULL;
	});

	if (stolen != NULL)
		return stolen;

	return NULL;
}


void
CPUEntry::StartQuantumTimer(ThreadData* thread, bool wasPreempted)
{
	cpu_ent* cpu = &gCPU[ID()];

	if (!wasPreempted || fUpdateLoadEvent)
		cancel_timer(&cpu->quantum_timer);
	fUpdateLoadEvent = false;

	if (!thread->IsIdle()) {
		bigtime_t quantum = thread->GetQuantumLeft();
		add_timer(&cpu->quantum_timer, &CPUEntry::_RescheduleEvent, quantum,
			B_ONE_SHOT_RELATIVE_TIMER);
	} else if (gTrackCoreLoad) {
		add_timer(&cpu->quantum_timer, &CPUEntry::_UpdateLoadEvent,
			kLoadMeasureInterval << 1, B_ONE_SHOT_RELATIVE_TIMER);
		fUpdateLoadEvent = true;
	}
}


void
CPUEntry::_RequestPerformanceLevel(ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	if (gCPU[fCPUNumber].disabled) {
		decrease_cpu_performance(kCPUPerformanceScaleMax);
		return;
	}

	int32 load = max_c(threadData->GetLoad(), fCore->GetLoad());
	ASSERT_PRINT(load >= 0 && load <= kMaxLoad, "load is out of range %"
		B_PRId32 " (max of %" B_PRId32 " %" B_PRId32 ")", load,
		threadData->GetLoad(), fCore->GetLoad());

	if (load < kTargetLoad) {
		int32 delta = kTargetLoad - load;

		delta *= kTargetLoad;
		delta /= kCPUPerformanceScaleMax;

		decrease_cpu_performance(delta);
	} else {
		int32 delta = load - kTargetLoad;
		delta *= kMaxLoad - kTargetLoad;
		delta /= kCPUPerformanceScaleMax;

		increase_cpu_performance(delta);
	}
}


/* static */ int32
CPUEntry::_RescheduleEvent(timer* /* unused */)
{
	get_cpu_struct()->invoke_scheduler = true;
	get_cpu_struct()->preempted = true;
	return B_HANDLED_INTERRUPT;
}


/* static */ int32
CPUEntry::_UpdateLoadEvent(timer* /* unused */)
{
	CoreEntry::GetCore(smp_get_current_cpu())->ChangeLoad(0);
	CPUEntry::GetCPU(smp_get_current_cpu())->fUpdateLoadEvent = false;
	return B_HANDLED_INTERRUPT;
}


CPUPriorityHeap::CPUPriorityHeap(int32 cpuCount)
	:
	Heap<CPUEntry, int32>(cpuCount)
{
}


void
CPUPriorityHeap::Dump()
{
	kprintf("cpu priority load\n");
	CPUEntry* entry = PeekRoot();
	while (entry) {
		int32 cpu = entry->ID();
		int32 key = GetKey(entry);
		kprintf("%3" B_PRId32 " %8" B_PRId32 " %3" B_PRId32 "%%\n", cpu, key,
			entry->GetLoad() / 10);

		RemoveRoot();
		sDebugCPUHeap.Insert(entry, key);

		entry = PeekRoot();
	}

	entry = sDebugCPUHeap.PeekRoot();
	while (entry) {
		int32 key = GetKey(entry);
		sDebugCPUHeap.RemoveRoot();
		Insert(entry, key);
		entry = sDebugCPUHeap.PeekRoot();
	}
}


CoreEntry::CoreEntry()
	:
	fPackage(NULL),
	fCPUCount(0),
	fCapacity(kDefaultCapacity),
	fIdleCPUCount(0),
	fThreadCount(0),
	fActiveTime(0),
	fLoad(0),
	fCurrentLoad(0),
	fLoadMeasurementEpoch(0),
	fLastLoadUpdate(0)
{
	B_INITIALIZE_SPINLOCK(&fCPULock);
	B_INITIALIZE_SPINLOCK(&fQueueLock);
}


void
CoreEntry::Init(int32 id, PackageEntry* package)
{
	fCoreID = id;
	fPackage = package;

	fCPUHeap.~CPUPriorityHeap();
	new(&fCPUHeap) CPUPriorityHeap(smp_get_num_cpus());
}


void
CoreEntry::PushFront(ThreadData* thread, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();

	fRunQueue.PushFront(thread, priority);
	atomic_add(&fThreadCount, 1);
}


void
CoreEntry::PushBack(ThreadData* thread, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();

	fRunQueue.PushBack(thread, priority);
	atomic_add(&fThreadCount, 1);
}


void
CoreEntry::Remove(ThreadData* thread)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!thread->IsIdle());

	ASSERT(thread->IsEnqueued());
	thread->SetDequeued();

	fRunQueue.Remove(thread);
	atomic_add(&fThreadCount, -1);
}


ThreadData*
CoreEntry::StealThread(int32& stolenPriority, int32 thiefCPU)
{
	SCHEDULER_ENTER_FUNCTION();

	ThreadData* thread = fRunQueue.PeekOption([&](ThreadData* thread) {
		const CPUSet& rawMask = thread->GetThread()->cpumask;
		if (rawMask.GetBit(thiefCPU))
			return true;
		if (rawMask.IsEmpty())
			return true;

		return rawMask.And(gCPUEnabled).IsEmpty();
	});

	if (thread != NULL) {
		stolenPriority = thread->GetEffectivePriority();
		Remove(thread);
	}
	return thread;
}


void
CoreEntry::AddCPU(CPUEntry* cpu)
{
	ASSERT(fCPUCount >= 0);
	ASSERT(atomic_get(&fIdleCPUCount) >= 0);

	atomic_add(&fIdleCPUCount, 1);
	if (atomic_add(&fCPUCount, 1) == 0) {
		// core has been reenabled
		fLoad = 0;
		fCurrentLoad = 0;

		atomic_set(&fPackage->fCoreLoads[fPackageIndex], 0);
		atomic_or((int32*)&fPackage->fEnabledCoreMask, 1U << fPackageIndex);

		fPackage->AddIdleCore(this);
	}
	fCPUSet.SetBit(cpu->ID());

	if (fCPUHeap.Insert(cpu, B_IDLE_PRIORITY) != B_OK)
		panic("CoreEntry::AddCPU: failed to insert CPU into heap");
}


void
CoreEntry::RemoveCPU(CPUEntry* cpu, ThreadProcessing& threadPostProcessing)
{
	ASSERT(fCPUCount > 0);
	ASSERT(atomic_get(&fIdleCPUCount) > 0);

	atomic_add(&fIdleCPUCount, -1);
	fCPUSet.ClearBit(cpu->ID());
	if (atomic_add(&fCPUCount, -1) == 1) {
		// unassign threads
		thread_map(CoreEntry::_UnassignThread, this);

		// core has been disabled
		atomic_and((int32*)&fPackage->fEnabledCoreMask, ~(1U << fPackageIndex));
		fPackage->RemoveIdleCore(this);

		// get rid of threads
		while (true) {
			ThreadData* threadData;
			{
				CoreRunQueueLocker locker(this);
				threadData = fRunQueue.PeekMaximum();
				if (threadData == NULL)
					break;

				Remove(threadData);
			}

			ASSERT(threadData->Core() == NULL);
			threadPostProcessing(threadData);
		}

		atomic_set(&fThreadCount, 0);
	}

	fCPUHeap.ModifyKey(cpu, -1);
	ASSERT(fCPUHeap.PeekRoot() == cpu);
	fCPUHeap.RemoveRoot();

	ASSERT(cpu->GetLoad() >= 0 && cpu->GetLoad() <= kMaxLoad);
	ASSERT(fLoad >= 0);
}


bigtime_t
CPUEntry::GetMinVirtualRuntime() const
{
	SCHEDULER_ENTER_FUNCTION();

	CPURunQueueLocker locker(const_cast<CPUEntry*>(this));
	ThreadData* thread = fRunQueue.PeekMaximum();
	if (thread == NULL)
		return 0;
	return thread->GetVirtualRuntime();
}


bigtime_t
CoreEntry::GetMinVirtualRuntime() const
{
	SCHEDULER_ENTER_FUNCTION();

	CoreRunQueueLocker locker(const_cast<CoreEntry*>(this));
	ThreadData* thread = fRunQueue.PeekMaximum();
	if (thread == NULL)
		return 0;
	return thread->GetVirtualRuntime();
}


void
CoreEntry::_UpdateLoad(bool forceUpdate)
{
	SCHEDULER_ENTER_FUNCTION();

	int32 cpuCount = atomic_get((int32*)&fCPUCount);
	if (cpuCount <= 0)
		return;

	bigtime_t now = system_time();
	bool intervalEnded = now >= kLoadMeasureInterval + fLastLoadUpdate;

	if (!intervalEnded && !forceUpdate)
		return;

	if (intervalEnded) {
		// No locking needed for atomic updates of fLoad.
		// fCurrentLoad is updated atomically.
		fLoad = fCurrentLoad;
		if (cpuCount > 0) {
			int32 load = fLoad / cpuCount;
			load = (int64)load * kDefaultCapacity / fCapacity;
			atomic_set(&fPackage->fCoreLoads[fPackageIndex],
				std::min(load, kMaxLoad));
		}
		fLoadMeasurementEpoch++;
		fLastLoadUpdate = now;
	}
}


/* static */ void
CoreEntry::_UnassignThread(Thread* thread, void* data)
{
	CoreEntry* core = static_cast<CoreEntry*>(data);
	ThreadData* threadData = thread->scheduler_data;

	if (threadData->Core() == core)
		threadData->UnassignCore();
}


SchedulerNode::SchedulerNode()
	:
	fIdlePackageMask(0),
	fPackageStartIndex(0),
	fPackageCount(0)
{
}


void
SchedulerNode::Init(int32 id)
{
	fNodeID = id;
	fIdlePackageMask = 0;
	fPackageStartIndex = 0;
	fPackageCount = 0;
}


PackageEntry::PackageEntry()
	:
	fIdleCoreCount(0),
	fCoreCount(0),
	fRegisteredCoreCount(0)
{
	B_INITIALIZE_RW_SPINLOCK(&fCoreLock);
}


void
PackageEntry::Init(int32 id, SchedulerNode* node, int32 nodeIndex)
{
	fPackageID = id;
	fNode = node;
	fNodeIndex = nodeIndex;
	fIdleCoreMask = 0;
	fEnabledCoreMask = 0;
	fCoreCount = 0;
	fRegisteredCoreCount = 0;
	memset(fCores, 0, sizeof(fCores));
	memset(fCoreLoads, 0, sizeof(fCoreLoads));
}


void
PackageEntry::AddIdleCore(CoreEntry* core)
{
	WriteSpinLocker coreLocker(fCoreLock);
	fCoreCount++;
	atomic_add(&fIdleCoreCount, 1);
	int32 oldMask = atomic_or((int32*)&fIdleCoreMask, 1U << core->PackageIndex());

	if (oldMask == 0)
		fNode->PackageGoesIdle(this);
}


void
PackageEntry::RemoveIdleCore(CoreEntry* core)
{
	WriteSpinLocker coreLocker(fCoreLock);
	int32 oldMask = atomic_and((int32*)&fIdleCoreMask, ~(1U << core->PackageIndex()));
	atomic_add(&fIdleCoreCount, -1);
	fCoreCount--;

	if ((oldMask & ~(1U << core->PackageIndex())) == 0)
		fNode->PackageWakesUp(this);
}


CoreEntry*
PackageEntry::GetIdleCore(int32 index) const
{
	uint32 mask = atomic_get((int32*)&fIdleCoreMask);
	int32 firstBit = -1;

	// Find the N-th set bit (index-th)
	for (int32 i = 0; i <= index; i++) {
		if (mask == 0)
			return NULL;

		firstBit = __builtin_ctz(mask);
		mask &= ~(1U << firstBit);
	}

	if (firstBit != -1)
		return fCores[firstBit];

	return NULL;
}


void
PackageEntry::RegisterCore(int32 index, CoreEntry* core)
{
	ASSERT(index >= 0 && index < kMaxCoresPerPackage);
	fCores[index] = core;
	fRegisteredCoreCount = max_c(fRegisteredCoreCount, index + 1);
}


CoreEntry*
PackageEntry::PeekMinimumLoadCore(const CPUSet* mask) const
{
	CoreEntry* minEntry = NULL;
	int32 minLoad = -1;

	// Use "Power of Two Choices" random sampling if the core count is large.
	// This avoids cache pollution and interconnect saturation from scanning all cores.
	if (fCoreCount > 8) {
		uint32 enabledMask = atomic_get((int32*)&fEnabledCoreMask);
		if (enabledMask == 0)
			return NULL;

		int32 firstIndex = -1;
		int32 attempts = 0;
		int32 registeredCores = fRegisteredCoreCount;
		if (registeredCores <= 0)
			return NULL;

		// Try to pick two distinct random valid cores.
		// Use formula: 4 + (3 * log2(N)) / 2
		// For 32 cores: 4 + 7.5 = 11 attempts.
		const int kMaxAttempts = 4 + (3 * (31 - __builtin_clz(registeredCores))) / 2;

		while (attempts++ < kMaxAttempts) {
			// Select a random bit index based on registered cores to avoid sparse array slots
			int32 i = CPUEntry::GetCPU(smp_get_current_cpu())->GetRandom() % registeredCores;

			// Check if this core is enabled
			if (!((1U << i) & enabledMask))
				continue;

			CoreEntry* candidate = fCores[i];
			if (mask != NULL && !mask->GetBit(candidate->ID()))
				continue;

			int32 load = atomic_get(&fCoreLoads[i]);

			if (firstIndex == -1) {
				// First choice
				firstIndex = i;
				minEntry = candidate;
				minLoad = load;
			} else if (i != firstIndex) {
				// Second choice: compare and return the better one
				if (load < minLoad)
					return candidate;
				return minEntry;
			}
		}

		// Fallback to linear scan if sampling failed to find 2 valid cores quickly
		if (minEntry != NULL)
			return minEntry;
	}

	// Linear Scan (Robust Path for small clusters or fallback)
	uint32 enabledMask = atomic_get((int32*)&fEnabledCoreMask);
	while (enabledMask != 0) {
		int32 i = __builtin_ctz(enabledMask);
		enabledMask &= ~(1U << i);

		CoreEntry* candidate = fCores[i];
		if (mask != NULL && !mask->GetBit(candidate->ID()))
			continue;

		int32 load = atomic_get(&fCoreLoads[i]);
		if (minEntry == NULL || load < minLoad) {
			minLoad = load;
			minEntry = candidate;
		}
	}
	return minEntry;
}


CoreEntry*
PackageEntry::PeekMaximumLoadCore(const CPUSet* mask) const
{
	CoreEntry* maxEntry = NULL;
	int32 maxLoad = -1;

	uint32 enabledMask = atomic_get((int32*)&fEnabledCoreMask);
	while (enabledMask != 0) {
		int32 i = __builtin_ctz(enabledMask);
		enabledMask &= ~(1U << i);

		CoreEntry* candidate = fCores[i];
		if (mask != NULL && !mask->GetBit(candidate->ID()))
			continue;

		int32 load = atomic_get(&fCoreLoads[i]);
		if (maxEntry == NULL || load > maxLoad) {
			maxLoad = load;
			maxEntry = candidate;
		}
	}
	return maxEntry;
}


/* static */ void
DebugDumper::DumpCPURunQueue(CPUEntry* cpu)
{
	ThreadRunQueue::ConstIterator iterator = cpu->fRunQueue.GetConstIterator();

	if (iterator.HasNext()
		&& !thread_is_idle_thread(iterator.Next()->GetThread())) {
		kprintf("\nCPU %" B_PRId32 " run queue:\n", cpu->ID());
		cpu->fRunQueue.Dump();
	}
}


/* static */ void
DebugDumper::DumpCoreRunQueue(CoreEntry* core)
{
	core->fRunQueue.Dump();
}


/* static */ void
DebugDumper::DumpCoreEntryLoad(CoreEntry* entry)
{
	CoreThreadsData threadsData;
	threadsData.fCore = entry;
	threadsData.fLoad = 0;
	thread_map(DebugDumper::_AnalyzeCoreThreads, &threadsData);

	kprintf("%4" B_PRId32 " %11" B_PRId32 "%% %11" B_PRId32 "%% %11" B_PRId32
		"%% %7" B_PRId32 " %5" B_PRIu32 "\n", entry->ID(), entry->fLoad / 10,
		entry->fCurrentLoad / 10, threadsData.fLoad, entry->ThreadCount(),
		entry->fLoadMeasurementEpoch);
}


/* static */ void
DebugDumper::DumpIdleCoresInPackage(PackageEntry* package)
{
	kprintf("%-7" B_PRId32 " ", package->fPackageID);

	uint32 mask = package->IdleCoreMask();
	if (mask != 0) {
		bool first = true;
		while (mask != 0) {
			int32 bit = __builtin_ctz(mask);
			mask &= ~(1U << bit);

			CoreEntry* core = package->GetCore(bit);
			kprintf("%s%" B_PRId32, first ? "" : ", ", core->ID());
			first = false;
		}
	} else
		kprintf("-");
	kprintf("\n");
}

/* static */ void
DebugDumper::DumpPackageCores(PackageEntry* package)
{
	kprintf("Package %" B_PRId32 " Cores:\n", package->fPackageID);
	for (int32 i = 0; i < package->RegisteredCoreCount(); i++) {
		CoreEntry* core = package->GetCore(i);
		if (core != NULL) {
			DumpCoreEntryLoad(core);
		}
	}
}


/* static */ void
DebugDumper::_AnalyzeCoreThreads(Thread* thread, void* data)
{
	CoreThreadsData* threadsData = static_cast<CoreThreadsData*>(data);
	if (thread->scheduler_data->Core() == threadsData->fCore)
		threadsData->fLoad += thread->scheduler_data->GetLoad();
}


static int
dump_run_queue(int /* argc */, char** /* argv */)
{
	int32 cpuCount = smp_get_num_cpus();
	int32 coreCount = gCoreCount;

	for (int32 i = 0; i < coreCount; i++) {
		kprintf("%sCore %" B_PRId32 " run queue:\n", i > 0 ? "\n" : "", i);
		DebugDumper::DumpCoreRunQueue(&gCoreEntries[i]);
	}

	for (int32 i = 0; i < cpuCount; i++)
		DebugDumper::DumpCPURunQueue(&gCPUEntries[i]);

	return 0;
}


static int
dump_cpu_heap(int /* argc */, char** /* argv */)
{
	kprintf("core average_load current_load threads_load threads epoch\n");

	for (int32 i = 0; i < gPackageCount; i++) {
		kprintf("Package %" B_PRId32 ":\n", gPackageEntries[i].fPackageID);
		DebugDumper::DumpPackageCores(&gPackageEntries[i]);
		kprintf("\n");
	}

	for (int32 i = 0; i < gCoreCount; i++) {
		if (gCoreEntries[i].CPUCount() < 2)
			continue;

		kprintf("\nCore %" B_PRId32 " heap:\n", i);
		gCoreEntries[i].CPUHeap()->Dump();
	}

	return 0;
}


static int
dump_idle_cores(int /* argc */, char** /* argv */)
{
	kprintf("Idle packages:\n");
	uint64 nodeMask = gIdleNodeMask;

	if (nodeMask != 0) {
		kprintf("node package cores\n");

		while (nodeMask != 0) {
			int32 nodeIndex = __builtin_ctzll(nodeMask);
			nodeMask &= ~(1ULL << nodeIndex);

			uint64 packageMask = gSchedulerNodes[nodeIndex].IdlePackageMask();
			while (packageMask != 0) {
				int32 packageIndex = __builtin_ctzll(packageMask);
				packageMask &= ~(1ULL << packageIndex);

				int32 globalPackageIndex
					= gSchedulerNodes[nodeIndex].PackageStartIndex() + packageIndex;
				if (globalPackageIndex < gPackageCount) {
					kprintf("%-4" B_PRId32 " ", nodeIndex);
					DebugDumper::DumpIdleCoresInPackage(&gPackageEntries[globalPackageIndex]);
				}
			}
		}
	} else
		kprintf("No idle packages.\n");

	return 0;
}


void Scheduler::init_debug_commands()
{
	new(&sDebugCPUHeap) CPUPriorityHeap(smp_get_num_cpus());

	add_debugger_command_etc("run_queue", &dump_run_queue,
		"List threads in run queue", "\nLists threads in run queue", 0);
	if (!gSingleCore) {
		add_debugger_command_etc("cpu_heap", &dump_cpu_heap,
			"List CPUs in CPU priority heap",
			"\nList CPUs in CPU priority heap", 0);
		add_debugger_command_etc("idle_cores", &dump_idle_cores,
			"List idle cores", "\nList idle cores", 0);
	}
}
