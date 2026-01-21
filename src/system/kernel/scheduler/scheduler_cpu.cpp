/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */


#include "scheduler_cpu.h"

#include <util/AutoLock.h>

#include <algorithm>

#include "scheduler_thread.h"


namespace Scheduler {


CPUEntry* gCPUEntries;

CoreEntry* gCoreEntries;
int32 gCoreCount;

PackageEntry* gPackageEntries;
uint64 gIdlePackageMask = 0;
int32 gPackageCount;


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
	if (sharedThread == NULL && pinnedThread == NULL && oldThread == NULL)
		return NULL;

	int32 sharedPriority = -1;
	if (sharedThread != NULL)
		sharedPriority = sharedThread->GetEffectivePriority();

	int32 rest = std::max(pinnedPriority, sharedPriority);
	if (oldPriority > rest || (!putAtBack && oldPriority == rest))
		return oldThread;

	if (sharedPriority > pinnedPriority) {
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

	int32 load = std::max(threadData->GetLoad(), fCore->GetLoad());
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
	fCPUCount(0),
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
	B_INITIALIZE_SEQLOCK(&fActiveTimeLock);
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

	fCPUHeap.Insert(cpu, B_IDLE_PRIORITY);
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
		atomic_set(&fPackage->fCoreLoads[fPackageIndex], fLoad);
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


PackageEntry::PackageEntry()
	:
	fIdleCoreCount(0),
	fCoreCount(0)
{
	B_INITIALIZE_RW_SPINLOCK(&fCoreLock);
}


void
PackageEntry::Init(int32 id)
{
	fPackageID = id;
	fIdleCoreMask = 0;
	fEnabledCoreMask = 0;
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
		atomic_or64((int64*)&gIdlePackageMask, 1ULL << fPackageID);
}


void
PackageEntry::RemoveIdleCore(CoreEntry* core)
{
	WriteSpinLocker coreLocker(fCoreLock);
	int32 oldMask = atomic_and((int32*)&fIdleCoreMask, ~(1U << core->PackageIndex()));
	atomic_add(&fIdleCoreCount, -1);
	fCoreCount--;

	if ((oldMask & ~(1U << core->PackageIndex())) == 0)
		atomic_and64((int64*)&gIdlePackageMask, ~(1ULL << fPackageID));
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
}


CoreEntry*
PackageEntry::PeekMinimumLoadCore() const
{
	CoreEntry* minEntry = NULL;
	int32 minLoad = -1;

	uint32 mask = atomic_get((int32*)&fEnabledCoreMask);
	while (mask != 0) {
		int32 i = __builtin_ctz(mask);
		mask &= ~(1U << i);

		int32 load = atomic_get(&fCoreLoads[i]);
		if (minEntry == NULL || load < minLoad) {
			minLoad = load;
			minEntry = fCores[i];
		}
	}
	return minEntry;
}


CoreEntry*
PackageEntry::PeekMaximumLoadCore() const
{
	CoreEntry* maxEntry = NULL;
	int32 maxLoad = -1;

	uint32 mask = atomic_get((int32*)&fEnabledCoreMask);
	while (mask != 0) {
		int32 i = __builtin_ctz(mask);
		mask &= ~(1U << i);

		int32 load = atomic_get(&fCoreLoads[i]);
		if (maxEntry == NULL || load > maxLoad) {
			maxLoad = load;
			maxEntry = fCores[i];
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
	for (int32 i = 0; i < kMaxCoresPerPackage; i++) {
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
	uint64 mask = gIdlePackageMask;

	if (mask != 0) {
		kprintf("package cores\n");

		while (mask != 0) {
			int32 bit = __builtin_ctzll(mask);
			mask &= ~(1ULL << bit);
			DebugDumper::DumpIdleCoresInPackage(&gPackageEntries[bit]);
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

