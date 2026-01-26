/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */


#include "scheduler_cpu.h"

#include <util/AutoLock.h>
#include <util/Random.h>

#include "scheduler_thread.h"


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
	fUpdateLoadEvent(false)
{
	B_INITIALIZE_RW_SPINLOCK(&fSchedulerModeLock);
	B_INITIALIZE_SPINLOCK(&fQueueLock);
}


void
CPUEntry::Init(int32 id, CoreEntry* core)
{
	fCPUNumber = id;
	fCore = core;
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
	irq_assignment* irq
		= (irq_assignment*)list_get_first_item(&entry->irqs);
	while (irq != NULL) {
		locker.Unlock();

		assign_io_interrupt_to_cpu(irq->irq, -1);

		locker.Lock();
		irq = (irq_assignment*)list_get_first_item(&entry->irqs);
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

	ASSERT(!gCPU[fCPUNumber].disabled);

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

	if (sharedThread == NULL && pinnedThread == NULL && oldThread == NULL)
		return NULL;

	int32 sharedPriority = -1;
	if (sharedThread != NULL)
		sharedPriority = sharedThread->GetEffectivePriority();

	int32 rest = max_c(pinnedPriority, sharedPriority);
	if (oldPriority > rest || (!putAtBack && oldPriority == rest))
		return oldThread;

	if (sharedPriority > pinnedPriority) {
		if (sharedThread->Core() == fCore)
			fCore->Remove(sharedThread);
		return sharedThread;
	}

	coreLocker.Unlock();

	Remove(pinnedThread);
	return pinnedThread;
}


void
CPUEntry::TrackActivity(ThreadData* oldThreadData, ThreadData* nextThreadData)
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
	// We modulo by registeredCores to avoid wasting iterations on unassigned array slots.
	int32 startIndex = fast_get_random<uint32>() % registeredCores;

	for (int32 i = 0; i < registeredCores; i++) {
		int32 index = (startIndex + i) % registeredCores;
		CoreEntry* victim = package->GetCore(index);

		if (victim == NULL || victim == fCore || victim->CPUCount() == 0)
			continue;

		// Use TryLock to avoid contention
		if (victim->TryLockRunQueue()) {
			int32 stolenPriority = -1;
			ThreadData* stolen = victim->StealThread(stolenPriority, fCPUNumber);
			victim->UnlockRunQueue();

			if (stolen != NULL)
				return stolen;
		}
	}

	// Topology-Aware Extension: Try to steal from sibling packages in the same
	// NUMA/Cluster node. This allows load balancing across the local interconnect
	// without incurring the high cost of a global search or remote node access.

	SchedulerNode* node = package->Node();
	if (node == NULL)
		return NULL;

	// Invert IdlePackageMask to find packages that have NO idle cores (fully busy)
	uint64 busyPackageMask = ~node->IdlePackageMask();

	// Limit our search to valid packages within this node
	// (Node 0 covers packages 0-63, Node 1 covers 64-127, etc.)
	int32 nodeBaseIndex = node->NodeIndex() * 64;
	int32 packagesInNode = min_c(64, gPackageCount - nodeBaseIndex);

	if (packagesInNode <= 0)
		return NULL;

	// Use random sampling to find a victim package instead of iterating the mask.
	// This ensures O(1) complexity for work stealing regardless of cluster size.
	// We scale the number of attempts slightly to improve hit rate on larger clusters,
	// using a logarithmic scale: attempts = 4 + log2(packagesInNode).
	// For 64 packages, attempts = 10. For 1 package, attempts = 4.
	const int kMaxPackageStealAttempts = 4 + (31 - __builtin_clz(packagesInNode));

	for (int i = 0; i < kMaxPackageStealAttempts; i++) {
		// Pick a random package index within this node
		int32 bit = fast_get_random<uint32>() % packagesInNode;

		// Check if the random package is busy (bit is set in mask)
		// Note: busyPackageMask is ~IdlePackageMask. If a package is idle, bit is 0.
		// If a package is busy, bit is 1. If package is invalid, bit is 1 (initially 0 in IdleMask).
		// However, we bounded 'bit' by 'packagesInNode', so packageIndex is guaranteed valid.
		if ((busyPackageMask & (1ULL << bit)) == 0)
			continue;

		int32 packageIndex = nodeBaseIndex + bit;

		// Skip our own package (already checked)
		if (packageIndex == package->fPackageID)
			continue;

		PackageEntry* victimPackage = &gPackageEntries[packageIndex];
		int32 victimCoreCount = victimPackage->RegisteredCoreCount();
		if (victimCoreCount == 0)
			continue;

		// Try to steal from a random busy core in the victim package
		// We make a few attempts to find a valid, busy, and unlocked core
		const int kMaxStealAttempts = 4;
		int32 attempts = 0;
		while (attempts++ < kMaxStealAttempts) {
			int32 coreIndex = fast_get_random<uint32>() % victimCoreCount;
			CoreEntry* victim = victimPackage->GetCore(coreIndex);

			// Skip invalid cores
			if (victim == NULL)
				continue;

			// Skip idle cores (check IdleCoreMask directly to avoid locking)
			uint32 idleMask = victimPackage->IdleCoreMask();
			if ((idleMask & (1U << victim->PackageIndex())) != 0)
				continue;

			if (victim->TryLockRunQueue()) {
				int32 stolenPriority = -1;
				ThreadData* stolen = victim->StealThread(stolenPriority, fCPUNumber);
				victim->UnlockRunQueue();

				if (stolen != NULL)
					return stolen;
			}
		}
	}

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

	ThreadData* thread = fRunQueue.PeekMaximum();
	if (thread != NULL) {
		CPUSet mask = thread->GetCPUMask();
		if (!mask.IsEmpty() && !mask.GetBit(thiefCPU))
			return NULL;

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
	if (fCPUCount++ == 0) {
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
	if (--fCPUCount == 0) {
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

	ThreadData* thread = fRunQueue.PeekMaximum();
	if (thread == NULL)
		return 0;
	return thread->GetVirtualRuntime();
}


bigtime_t
CoreEntry::GetMinVirtualRuntime() const
{
	SCHEDULER_ENTER_FUNCTION();

	ThreadData* thread = fRunQueue.PeekMaximum();
	if (thread == NULL)
		return 0;
	return thread->GetVirtualRuntime();
}


void
CoreEntry::_UpdateLoad(bool forceUpdate)
{
	SCHEDULER_ENTER_FUNCTION();

	if (fCPUCount <= 0)
		return;

	bigtime_t now = system_time();
	bool intervalEnded = now >= kLoadMeasureInterval + fLastLoadUpdate;

	if (!intervalEnded && !forceUpdate)
		return;

	if (intervalEnded) {
		// No locking needed for atomic updates of fLoad.
		// fCurrentLoad is updated atomically.
		fLoad = fCurrentLoad;
		if (fCPUCount > 0) {
			int32 load = fLoad / fCPUCount;
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
	fIdlePackageMask(0)
{
}


void
SchedulerNode::Init(int32 id)
{
	fNodeID = id;
	fIdlePackageMask = 0;
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
PackageEntry::Init(int32 id, SchedulerNode* node)
{
	fPackageID = id;
	fNode = node;
	fNodeIndex = id % 64; // Assuming 64 packages per node max
	fIdleCoreMask = 0;
	fEnabledCoreMask = 0;
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

		// Try to pick two distinct random valid cores.
		// We limit attempts to avoid infinite loops if the mask is sparse.
		while (attempts++ < 4) {
			// Select a random bit index based on registered cores to avoid sparse array slots
			int32 i = fast_get_random<uint32>() % registeredCores;

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

				int32 globalPackageIndex = nodeIndex * 64 + packageIndex;
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
