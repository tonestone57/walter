/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */

// Patch: Fix node->NodeIndex() calls, PeekMaximumLoadCore null deref,
// and double-modification of fIdlePackageMask in AddIdleCore / RemoveIdleCore.


#include "scheduler_cpu.h"

#include <new>

#include <interrupts.h>
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


void
IRQRebalanceDPC::DoDPC(DPCQueue* queue)
{
	assign_io_interrupt_to_cpu(fIRQ, fTargetCPU);
}


CPUEntry::CPUEntry()
	:
	fThreadCount(0),
	fLoad(0),
	fPerformanceScale(kDefaultCapacity),
	fMeasureActiveTime(0),
	fMeasureTime(0),
	fUpdateLoadEvent(false),
	fRandomState(1),
	fRescheduleCount(0)
{
	B_INITIALIZE_RW_SPINLOCK(&fSchedulerModeLock);
	B_INITIALIZE_SPINLOCK(&fQueueLock);
}


void
CPUEntry::Init(int32 id, CoreEntry* core)
{
	fCPUNumber = id;
	fCore = core;
	// Mix id into high bits to avoid correlation if system_time is similar
	// Step 1: Initial entropy mix
	uint64 seed = system_time() ^ ((uint64)id << 16) ^ ((uint64)id * 0xBF58476D1CE4E5B9ULL);
	// Step 2: Final mixing (using a different constant)
	seed = (seed ^ (seed >> 30)) * 0x94D049BB133111EBULL;
	fRandomState = seed ? seed : 1;
	// Stagger the boost-scan trigger across CPUs. Without this all CPUs
	// fire UpdatePriorityBoostScalable at the same reschedule boundary,
	// causing correlated lock acquisition and measurable latency spikes.
	fRescheduleCount = (uint32)(id % 10);

	fInteractionUpdateCounter = 0;
	fReschedulePending = 0;
	fLastLocalPackageIndex = 0;	// Fix #14
}


void
CPUEntry::Start()
{
	fThreadCount = 0;
	fLoad = 0;
	fMeasureTime = system_time();
	fMeasureActiveTime = 0;
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
		if (currentHead != NULL && currentHead->irq == irqVector) {
			dprintf("CPUEntry::Stop: interrupt %" B_PRId32 " still assigned "
				"after successful reassignment\n", irqVector);
			break;
		}
	}
	locker.Unlock();
}


void
CPUEntry::PushFront(ThreadData* thread, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();
	fRunQueue.PushFront(thread, priority);
	atomic_add(&fThreadCount, 1);
}


void
CPUEntry::PushBack(ThreadData* thread, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();
	fRunQueue.PushBack(thread, priority);
	atomic_add(&fThreadCount, 1);
}


void
CPUEntry::Remove(ThreadData* thread)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(thread->IsEnqueued());
	thread->SetDequeued();
	fRunQueue.Remove(thread);
	atomic_add(&fThreadCount, -1);
}


ThreadData*
CoreEntry::PeekThread() const
{
	SCHEDULER_ENTER_FUNCTION();
	return fRunQueue.PeekBest();
}


ThreadData*
CPUEntry::PeekThread() const
{
	SCHEDULER_ENTER_FUNCTION();
	return fRunQueue.PeekBest();
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
		Scheduler::RebalanceIRQs(false);
}


ThreadData*
CPUEntry::ChooseNextThread(ThreadData* oldThread, bool putAtBack)
{
	SCHEDULER_ENTER_FUNCTION();

	int32 oldPriority = -1;
	if (oldThread != NULL)
		oldPriority = oldThread->GetEffectivePriority();

	CPURunQueueLocker cpuLocker(this);

	ThreadData* pinnedThread = PeekThread();
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

		// Fix #6: Compute system_time() once and pass it to UpdateActivity so
		// the virtual-runtime ceiling uses the same timestamp as the rest of
		// this scheduling decision.  Avoids a redundant syscall on the hot path.
		oldThreadData->UpdateActivity(active, system_time());
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
	uint64 x = fRandomState;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	fRandomState = x;
	return (uint32)x;
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

	search_local_node(node, [this, package, &stolen](PackageEntry* entry) {
		// Note: 'stolen' is captured by reference and acts as a single-threaded
		// accumulator for this CPU. This is safe because _TryStealWork is only
		// ever called by the CPU's own rescheduling loop.
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

		if ((entry->IdleCoreMask() & ((native_cpu_mask_t)1 << victim->PackageIndex())) != 0)
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

	search_global_random([this, package, &stolen](PackageEntry* entry) {
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

		if ((entry->IdleCoreMask() & ((native_cpu_mask_t)1 << victim->PackageIndex())) != 0)
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
	fType(CORE_TYPE_UNKNOWN),
	fCPUCount(0),
	fCapacity(kDefaultCapacity),
	fIdleCPUCount(0),
	fThreadCount(0),
	fTotalThreadCount(0),
	fActiveTime(0),
	fLoad(0),
	fCombinedLoad(0),
	fLastLoadUpdate(0),
	fScoreFactor(1 << 16)
{
	B_INITIALIZE_SPINLOCK(&fCPULock);
	B_INITIALIZE_SPINLOCK(&fQueueLock);
}


void
CoreEntry::Init(int32 id, PackageEntry* package)
{
	fCoreID = id;
	fPackage = package;

	fScoreFactor = (kDefaultCapacity << 16) / fCapacity;

	fCPUHeap.~CPUPriorityHeap();
	new(&fCPUHeap) CPUPriorityHeap(smp_get_num_cpus());
	if (fCPUHeap.InitCheck() != B_OK)
		panic("CoreEntry::Init: failed to allocate CPU heap");
}


void
CoreEntry::PushFront(ThreadData* thread, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();

	fRunQueue.PushFront(thread, priority);
	atomic_add(&fThreadCount, 1);
	atomic_add(&fTotalThreadCount, 1);
}


void
CoreEntry::PushBack(ThreadData* thread, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();

	fRunQueue.PushBack(thread, priority);
	atomic_add(&fThreadCount, 1);
	atomic_add(&fTotalThreadCount, 1);
}


void
CoreEntry::Remove(ThreadData* thread)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!thread->IsIdle());

	ASSERT(thread->IsEnqueued());
	thread->SetDequeued();

	atomic_add(&fThreadCount, -1);
	atomic_add(&fTotalThreadCount, -1);
	fRunQueue.Remove(thread);
}


ThreadData*
CoreEntry::StealThread(int32& stolenPriority, int32 thiefCPU)
{
	SCHEDULER_ENTER_FUNCTION();

	ThreadData* thread = fRunQueue.PeekOption([&](ThreadData* thread) {
		const CPUSet& mask = thread->GetCPUMask();
		if (mask.GetBit(thiefCPU))
			return true;
		if (mask.IsEmpty())
			return true;

		return false;
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
	bool firstCPU = (atomic_add(&fCPUCount, 1) == 0);
	if (firstCPU) {
		// core has been reenabled
		fLoad = 0;
		atomic_set64(&fCombinedLoad, 0);

		atomic_set(&fPackage->fCoreLoads[fPackageIndex], 0);
		scheduler_atomic_or(&fPackage->fEnabledCoreMask,
			(native_cpu_mask_t)1 << fPackageIndex);

		fPackage->AddIdleCore(this);
	}
	fCPUSet.SetBitAtomic(cpu->ID());

	if (fCPUHeap.Insert(cpu, B_IDLE_PRIORITY) != B_OK) {
		// Roll back all state changes made above so the post-panic debugger
		// does not see a CPU that appears initialised but has no heap entry.
		// Fix #8 (documentation): The rollback is correct as written.
		// RemoveIdleCore() decrements PackageEntry::fIdleCoreCount (a per-
		// package idle-core counter).  The atomic_add at the bottom decrements
		// CoreEntry::fIdleCPUCount (a per-core idle-CPU counter).  These are
		// entirely different variables; there is no double-decrement of any
		// single counter.  This comment guards against "simplification" that
		// would merge the two undo operations and break one of them.
		fCPUSet.ClearBitAtomic(cpu->ID());
		if (firstCPU) {
			fPackage->RemoveIdleCore(this);
			scheduler_atomic_and(&fPackage->fEnabledCoreMask,
				~((native_cpu_mask_t)1 << fPackageIndex));
			// Restore fCPUCount and fIdleCPUCount symmetrically.
			atomic_add(&fCPUCount, -1);
		} else {
			atomic_add(&fCPUCount, -1);
		}
		atomic_add(&fIdleCPUCount, -1);
		panic("CoreEntry::AddCPU: failed to insert CPU %" B_PRId32 " into heap",
			cpu->ID());
	}
}


void
CoreEntry::RemoveCPU(CPUEntry* cpu, ThreadProcessing& threadPostProcessing)
{
	ASSERT(fCPUCount > 0);
	ASSERT(atomic_get(&fIdleCPUCount) > 0);

	// Decrement fIdleCPUCount unconditionally: AddCPU always increments it
	// (every CPU starts idle), so RemoveCPU must balance it regardless of
	// whether the CPU is currently idle or running at removal time.
	atomic_add(&fIdleCPUCount, -1);
	fCPUSet.ClearBitAtomic(cpu->ID());
	if (atomic_add(&fCPUCount, -1) == 1) {
		// core has been disabled
		scheduler_atomic_and(&fPackage->fEnabledCoreMask,
			~((native_cpu_mask_t)1 << fPackageIndex));
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

		// Do NOT zero fThreadCount here.  Each Remove() call above already
		// decrements it, and once fCPUCount reaches zero enqueue() refuses to
		// add new threads to this core (CPUCount() == 0 guard), so the count
		// is already zero after the drain loop.  Forcing it to zero here would
		// clobber any threads that race in between Remove() and this line,
		// even though in practice that cannot happen today, removing this line
		// eliminates the latent hazard.
	}

	// Fix #4: Use INT32_MIN instead of the implicit magic -1.  INT32_MIN is
	// less than every valid scheduler priority (minimum B_IDLE_PRIORITY == 0),
	// so the CPU is guaranteed to bubble to the heap root.  The explicit
	// constant makes the intent clear and prevents silent misbehaviour if the
	// priority range ever grows to include negative values.
	fCPUHeap.ModifyKey(cpu, INT32_MIN);
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
	ThreadData* thread = fRunQueue.PeekBest(ThreadDataVRuntimeCompare(),
		ThreadDataOptimal());
	if (thread == NULL)
		return 0;
	return thread->GetVirtualRuntime();
}


bigtime_t
CoreEntry::GetMinVirtualRuntime() const
{
	SCHEDULER_ENTER_FUNCTION();

	CoreRunQueueLocker locker(const_cast<CoreEntry*>(this));
	ThreadData* thread = fRunQueue.PeekBest(ThreadDataVRuntimeCompare(),
		ThreadDataOptimal());
	if (thread == NULL)
		return 0;
	return thread->GetVirtualRuntime();
}


CPUEntry*
CoreEntry::PeekMinimumLoadCPU()
{
	// Optimization: If a physical core is idle, explicitly return the
	// Primary logical CPU first. This prevents unnecessary resource
	// contention shared across SMT logical pairs (e.g. L1i/L1d).
	if (fCPUCount > 1 && GetScore() == 0) {
		// Find the first set bit in the CPU set to identify the primary logical CPU.
		for (int i = 0; i < (SMP_MAX_CPUS + 31) / 32; i++) {
			uint32 bits = fCPUSet.Bits(i);
			if (bits != 0) {
				int cpu = i * 32 + (__builtin_ffs(bits) - 1);
				return &gCPUEntries[cpu];
			}
		}
	}

	CoreCPUHeapLocker _(this);
	return fCPUHeap.PeekRoot();
}


void
CoreEntry::SetCapacity(int32 capacity)
{
	fCapacity = capacity;
	fScoreFactor = (kDefaultCapacity << 16) / fCapacity;
}


void
CoreEntry::_UpdateLoad(bool forceUpdate)
{
	SCHEDULER_ENTER_FUNCTION();

	int32 cpuCount = atomic_get((int32*)&fCPUCount);
	if (cpuCount <= 0)
		return;

	bigtime_t now = system_time();
	bigtime_t lastUpdate = atomic_get64(&fLastLoadUpdate);
	bool intervalEnded = now >= kLoadMeasureInterval + lastUpdate;

	if (intervalEnded || forceUpdate) {
		if (atomic_test_and_set64(&fLastLoadUpdate, now, lastUpdate)
				!= lastUpdate) {
			return;
		}
	} else
		return;

	// Combined Epoch and Load Update:
	// We use a 64-bit atomic to store both the current load (upper 32 bits)
	// and the measurement epoch (lower 32 bits). This allows us to atomically
	// increment the epoch and reset the current load, ensuring that concurrent
	// AddLoad calls either contribute to the old epoch (and are manually
	// added to fLoad) or the new epoch (and are captured in the next snapshot),
	// with no double-counting or loss.
	int32 currentLoad = 0;
	int64 oldCombined = atomic_get64(&fCombinedLoad);
	while (true) {
		currentLoad = (int32)(oldCombined >> 32);
		uint32 nextEpoch = (uint32)oldCombined + 1;
		int64 newCombined = (int64)nextEpoch; // Load reset to 0

		int64 actual = atomic_test_and_set64(&fCombinedLoad, newCombined,
			oldCombined);
		if (actual == oldCombined) {
			// Read fLoad AFTER the CAS. Concurrent AddLoad() calls that detect the
			// epoch change AFTER the CAS will do atomic_add(&fLoad, load) directly.
			// By reading it here, we ensure that any load added to fLoad before
			// we reset fCombinedLoad is accounted for in our delta calculation.
			int32 prevLoad = atomic_get(&fLoad);

			// Use atomic_add rather than atomic_set.  Between the CAS above
			// (which resets the upper 32 bits of fCombinedLoad to 0 and bumps
			// the epoch) and here, concurrent AddLoad() calls that detect the
			// epoch change do atomic_add(&fLoad, load) directly.  An atomic_set
			// here would silently overwrite those additions, dropping load
			// contributions for threads that woke up in this window.
			//
			// Instead, compute the delta from the last snapshotted value and
			// add it atomically.
			int32 delta = currentLoad - prevLoad;
			if (delta != 0)
				atomic_add(&fLoad, delta);
			break;
		}
		oldCombined = actual;
	}

	if (cpuCount > 0) {
		int32 load = currentLoad / cpuCount;
		load = ((int64)load * fScoreFactor) >> 16;
		atomic_set(&fPackage->fCoreLoads[fPackageIndex],
			min_c(load, (int32)kMaxLoad));
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
	native_cpu_mask_t oldMask = scheduler_atomic_or(&fIdleCoreMask,
		(native_cpu_mask_t)1 << core->PackageIndex());
	atomic_add(&fIdleCoreCount, 1);

	if (oldMask == 0) {
		// Package goes idle (first idle core).  Delegate entirely to
		// PackageGoesIdle so that fIdlePackageMask and gIdleNodeMask are
		// updated in exactly one place, matching the CoreGoesIdle path.
		// The previous code called SetPackageIdle AND PackageGoesIdle,
		// which double-wrote fIdlePackageMask; PackageGoesIdle then saw a
		// non-zero oldMask and never updated gIdleNodeMask.  It also
		// called the nonexistent node->Index() causing a compile error.
		if (fNode != NULL)
			fNode->PackageGoesIdle(this);
	}
}


void
PackageEntry::RemoveIdleCore(CoreEntry* core)
{
	WriteSpinLocker coreLocker(fCoreLock);
	// Decrement the count BEFORE clearing the mask bit.  A concurrent reader
	// (e.g. GetLeastIdlePackage) checks fIdleCoreCount as a fast pre-filter
	// before inspecting fIdleCoreMask.  If the mask is cleared first, the
	// reader can observe fIdleCoreCount > 0 with an empty mask and then
	// dereference a NULL core from GetIdleCore().
	atomic_add(&fIdleCoreCount, -1);

	native_cpu_mask_t clearBit = (native_cpu_mask_t)1 << core->PackageIndex();
	native_cpu_mask_t oldMask = scheduler_atomic_and(&fIdleCoreMask, ~clearBit);

	if ((oldMask & ~clearBit) == 0) {
		// Package wakes up (last idle core became active).  Delegate to
		// PackageWakesUp so that fIdlePackageMask and gIdleNodeMask are
		// updated in exactly one place.
		// The previous code called SetPackageIdle (which cleared the bit in
		// fIdlePackageMask) and then PackageWakesUp; PackageWakesUp received
		// oldMask == 0 from an already-cleared mask and unconditionally
		// cleared gIdleNodeMask even when other packages in the node were
		// still idle.  It also called the nonexistent node->Index().
		if (fNode != NULL)
			fNode->PackageWakesUp(this);
	}
}


CoreEntry*
PackageEntry::GetIdleCore(int32 index) const
{
	native_cpu_mask_t mask = scheduler_atomic_get(&fIdleCoreMask);
	int32 firstBit = -1;

	// Find the N-th set bit (index-th)
	for (int32 i = 0; i <= index; i++) {
		if (mask == 0)
			return NULL;

		firstBit = scheduler_ctz(mask);
		mask &= ~((native_cpu_mask_t)1 << firstBit);
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
	fCoreCount++;
	fRegisteredCoreCount = max_c(fRegisteredCoreCount, index + 1);
}


CoreEntry*
PackageEntry::PeekMinimumLoadCore(CPUEntry* cpu, const CPUSet* mask,
	CoreType type) const
{
	CoreEntry* minEntry = NULL;
	int32 minLoad = -1;

	native_cpu_mask_t enabledMask = scheduler_atomic_get((native_cpu_mask_t*)&fEnabledCoreMask);
	if (enabledMask == 0)
		return NULL;

	// Use "Power of Two Choices" random sampling if the core count is large.
	// This avoids cache pollution and interconnect saturation from scanning all cores.
	if (fRegisteredCoreCount > kRandomCoreSearchThreshold) {	// Fix #15
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
			int32 i = (int32)(((uint64)cpu->GetRandom() * registeredCores) >> 32);

			CoreEntry* candidate = fCores[i];
			if (candidate == NULL)
				continue;

			if (i == firstIndex)
				continue;

			// Check if this core is enabled
			if (!(((native_cpu_mask_t)1 << i) & enabledMask))
				continue;

			if (firstIndex == -1)
				firstIndex = i;

			if (mask != NULL && !mask->GetBit(candidate->ID()))
				continue;
			if (type != CORE_TYPE_UNKNOWN && candidate->Type() != type)
				continue;

			int32 load = atomic_get(&fCoreLoads[i]);

			// Track the best core across all attempts (Power-of-N-Choices).
			if (minEntry == NULL || load < minLoad) {
				minLoad = load;
				minEntry = candidate;
			}
		}

		// Use the best sampled core if any were found
		if (minEntry != NULL)
			return minEntry;
	}

	// Linear Scan (Robust Path for small clusters or fallback)
	native_cpu_mask_t currentEnabledMask = enabledMask;
	while (currentEnabledMask != 0) {
		int32 i = scheduler_ctz(currentEnabledMask);
		currentEnabledMask &= ~((native_cpu_mask_t)1 << i);

		CoreEntry* candidate = fCores[i];
		if (candidate == NULL)
			continue;
		if (mask != NULL && !mask->GetBit(candidate->ID()))
			continue;
		if (type != CORE_TYPE_UNKNOWN && candidate->Type() != type)
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
PackageEntry::PeekMaximumLoadCore(CPUEntry* cpu, const CPUSet* mask,
	CoreType type) const
{
	CoreEntry* maxEntry = NULL;
	int32 maxLoad = -1;

	native_cpu_mask_t enabledMask = scheduler_atomic_get((native_cpu_mask_t*)&fEnabledCoreMask);
	if (enabledMask == 0)
		return NULL;

	// Use "Power of Two Choices" random sampling if the core count is large.
	// This avoids cache pollution and interconnect saturation from scanning all cores.
	if (fRegisteredCoreCount > kRandomCoreSearchThreshold) {	// Fix #15
		int32 firstIndex = -1;
		int32 attempts = 0;
		int32 registeredCores = fRegisteredCoreCount;
		if (registeredCores <= 0)
			return NULL;

		// Try to pick two distinct random valid cores.
		// Use formula: 4 + (3 * log2(N)) / 2
		const int kMaxAttempts = 4 + (3 * (31 - __builtin_clz(registeredCores))) / 2;

		while (attempts++ < kMaxAttempts) {
			// Select a random bit index based on registered cores
			int32 i = (int32)(((uint64)cpu->GetRandom() * registeredCores) >> 32);

			CoreEntry* candidate = fCores[i];
			if (candidate == NULL)
				continue;

			if (i == firstIndex)
				continue;

			// Check if this core is enabled
			if (!(((native_cpu_mask_t)1 << i) & enabledMask))
				continue;

			if (firstIndex == -1)
				firstIndex = i;

			if (mask != NULL && !mask->GetBit(candidate->ID()))
				continue;
			if (type != CORE_TYPE_UNKNOWN && candidate->Type() != type)
				continue;

			int32 load = atomic_get(&fCoreLoads[i]);

			// Track the best core across all attempts (Power-of-N-Choices).
			if (maxEntry == NULL || load > maxLoad) {
				maxLoad = load;
				maxEntry = candidate;
			}
		}

		// Use the best sampled core if any were found
		if (maxEntry != NULL)
			return maxEntry;
	}

	// Linear Scan (Robust Path for small clusters or fallback)
	int32 count = scheduler_popcount(enabledMask);
	int32 startBit = 0;

	if (count > 1) {
		startBit = (int32)(((uint64)cpu->GetRandom() * kMaxCoresPerPackage) >> 32);
	}

	// Split mask into two parts to randomize start position
	native_cpu_mask_t upperMask = enabledMask & (~(native_cpu_mask_t)0 << startBit);
	native_cpu_mask_t lowerMask = enabledMask & (((native_cpu_mask_t)1 << startBit) - 1);

	// We iterate twice, but effectively just once over the set bits.
	// The order of loops determines tie-breaking preference.

	for (int pass = 0; pass < 2; pass++) {
		native_cpu_mask_t currentMask = (pass == 0) ? upperMask : lowerMask;

		while (currentMask != 0) {
			int32 i = scheduler_ctz(currentMask);
			currentMask &= ~((native_cpu_mask_t)1 << i);

			CoreEntry* candidate = fCores[i];
			if (candidate == NULL)
				continue;
			if (mask != NULL && !mask->GetBit(candidate->ID()))
				continue;
			if (type != CORE_TYPE_UNKNOWN && candidate->Type() != type)
				continue;

			int32 load = atomic_get(&fCoreLoads[i]);
			if (maxEntry == NULL || load > maxLoad) {
				maxLoad = load;
				maxEntry = candidate;
			}
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
		entry->CurrentLoad() / 10, threadsData.fLoad, entry->ThreadCount(),
		entry->LoadMeasurementEpoch());
}


/* static */ void
DebugDumper::DumpIdleCoresInPackage(PackageEntry* package)
{
	kprintf("%-7" B_PRId32 " ", package->fPackageID);

	native_cpu_mask_t mask = package->IdleCoreMask();
	if (mask != 0) {
		bool first = true;
		while (mask != 0) {
			int32 bit = scheduler_ctz(mask);
			mask &= ~((native_cpu_mask_t)1 << bit);

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
	uint64 nodeMask = atomic_get64((int64*)&gIdleNodeMask);

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
	if (sDebugCPUHeap.InitCheck() != B_OK)
		panic("Scheduler::init_debug_commands: failed to allocate CPU heap");

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
