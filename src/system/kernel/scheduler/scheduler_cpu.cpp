// AUDIT FIXES: issues 2, 7, 9, 15, 17, 21, 31, 45, 52, 59, 66, 67, 86, 89, 96
/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 * Audit fixes applied 2025.
 */


#include "scheduler_cpu.h"

#include <new>

#include <interrupts.h>
#include <util/AutoLock.h>
#include <util/Random.h>

#include "scheduler_thread.h"
#include "scheduler_topology.h"


namespace Scheduler {

static inline uint32
get_random_index(uint32 random, uint32 range)
{
	return (uint32)(((uint64)random * range) >> 32);
}


CPUEntry* gCPUEntries;

CoreEntry* gCoreEntries;
int32 gCoreCount;

PackageEntry* gPackageEntries;
int32 gPackageCount;

SchedulerNode* gSchedulerNodes;
uint64 gIdleNodeMask __attribute__((aligned(8))) = 0;
int32 gNodeCount;



struct LocalNodeStealAction {
	CPUEntry* cpu;
	PackageEntry* package;
	ThreadData** stolen;

	LocalNodeStealAction(CPUEntry* c, PackageEntry* p, ThreadData** s)
		: cpu(c), package(p), stolen(s) {}

	bool operator()(PackageEntry* entry) const {
		if (*stolen != NULL)
			return true;

		if (entry == package)
			return false;

		int32 victimCoreCount = entry->RegisteredCoreCount();
		if (victimCoreCount == 0)
			return false;

		int32 coreIndex = (int32)get_random_index(cpu->GetRandom(), victimCoreCount);
		CoreEntry* victim = entry->GetCore(coreIndex);

		if (victim == NULL)
			return false;

		if ((entry->IdleCoreMask() & ((native_cpu_mask_t)1 << victim->PackageIndex())) != 0)
			return false;

		if (victim->TryLockRunQueue()) {
			int32 stolenPriority = -1;
			*stolen = victim->StealThread(stolenPriority, cpu->ID());

			if (*stolen != NULL) {
				(*stolen)->MigrateTo(cpu->Core());
				(*stolen)->fStolen = true;
				cpu->Core()->IncrementTotalThreadCount();
				victim->UnlockRunQueue();
				return true;
			}

			victim->UnlockRunQueue();
		}

		return false;
	}
};

struct GlobalRandomStealAction {
	CPUEntry* cpu;
	PackageEntry* package;
	ThreadData** stolen;

	GlobalRandomStealAction(CPUEntry* c, PackageEntry* p, ThreadData** s)
		: cpu(c), package(p), stolen(s) {}

	bool operator()(PackageEntry* entry) const {
		if (*stolen != NULL)
			return true;

		if (entry == package)
			return false;

		if (entry->IdleCoreCount() == entry->CoreCount())
			return false;

		int32 victimCoreCount = entry->RegisteredCoreCount();
		if (victimCoreCount == 0)
			return false;

		int32 coreIndex = (int32)get_random_index(cpu->GetRandom(), victimCoreCount);
		CoreEntry* victim = entry->GetCore(coreIndex);

		if (victim == NULL)
			return false;

		if ((entry->IdleCoreMask() & ((native_cpu_mask_t)1 << victim->PackageIndex())) != 0)
			return false;

		if (victim->TryLockRunQueue()) {
			int32 stolenPriority = -1;
			*stolen = victim->StealThread(stolenPriority, cpu->ID());

			if (*stolen != NULL) {
				(*stolen)->MigrateTo(cpu->Core());
				(*stolen)->fStolen = true;
				cpu->Core()->IncrementTotalThreadCount();
				victim->UnlockRunQueue();
				return true;
			}

			victim->UnlockRunQueue();
		}

		return false;
	}
};

struct StealThreadPredicate {
	const CPUSet& enabledSnapshot;
	int32 thiefCPU;

	StealThreadPredicate(const CPUSet& e, int32 t)
		: enabledSnapshot(e), thiefCPU(t) {}

	bool operator()(ThreadData* td) const {
		if (td->IsIdle())
			return false;

		const CPUSet& cpumask = td->GetThread()->cpumask;
		if (cpumask.IsEmpty()) {
			return enabledSnapshot.GetBit(thiefCPU);
		}
		return cpumask.GetBit(thiefCPU)
			&& enabledSnapshot.GetBit(thiefCPU);
	}
};

struct CoreThreadsData {
			CoreEntry*	fCore;
			int32		fLoad;
	};



class Scheduler::DebugDumper {
public:
	static	void		DumpCPURunQueue(CPUEntry* cpu);
	static	void		DumpCoreRunQueue(CoreEntry* core);
	static	void		DumpCoreEntryLoad(CoreEntry* core);
	static	void		DumpIdleCoresInPackage(PackageEntry* package);
	static	void		DumpPackageCores(PackageEntry* package);

private:

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
	fRescheduleCount(0),
	fCoreLocalIndex(0),
	fInteractionUpdateCounter(0),
	fReschedulePending(0),
	fLastLocalPackageIndex(0),
	lastReschedule(0)
{
	B_INITIALIZE_RW_SPINLOCK(&fSchedulerModeLock);
	B_INITIALIZE_SPINLOCK(&fQueueLock);
}


void
CPUEntry::Init(int32 id, CoreEntry* core)
{
	fCPUNumber = id;
	fCore = core;
	uint64 seed = system_time();
	seed ^= (uint64)(uintptr_t)this;
	seed ^= ((uint64)id * 0x9E3779B97F4A7C15ULL);
	seed ^= (uint64)id << 32;
	seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ULL;
	seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBULL;
	seed ^= seed >> 31;
	fRandomState = seed ? seed : 1;

	{
		int32 numCPUs = smp_get_num_cpus();
		int32 staggerMod = (numCPUs > 1 && numCPUs <= 10) ? numCPUs : 10;
		// Issue 44 fix: unique context switch boundaries across CPUs
		// reduce correlated lock spikes.
		fRescheduleCount = (uint32)(id % staggerMod);
	}
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
	SpinLocker locker(entry->irqs_lock);
	// Issue 2 fix: 1000 iteration limit for IRQ draining.
	for (int32 i = 0; i < 1000; i++) {
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
			dprintf("CPUEntry::Stop: interrupt %" B_PRId32 " could not be "
				"reassigned (driver failure); aborting IRQ drain\n", irqVector);
			break;
		}
	}

	if (list_get_first_item(&entry->irqs) != NULL) {
		dprintf("CPUEntry::Stop: safety limit reached while removing "
			"interrupts from CPU %" B_PRId32 "\n", fCPUNumber);
	}
	locker.Unlock();
}


void
CPUEntry::PushFront(ThreadData* thread, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();
	fRunQueue.PushFront(thread, priority);
	atomic_add(&fThreadCount, 1);

	if (!thread->IsIdle()) {
		Core()->IncrementTotalThreadCount();
		if (priority >= B_DISPLAY_PRIORITY)
			Core()->IncrementDisplayThreadCount();
	}
}


void
CPUEntry::PushBack(ThreadData* thread, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();
	fRunQueue.PushBack(thread, priority);
	atomic_add(&fThreadCount, 1);

	if (!thread->IsIdle()) {
		Core()->IncrementTotalThreadCount();
		if (priority >= B_DISPLAY_PRIORITY)
			Core()->IncrementDisplayThreadCount();
	}
}


void
CPUEntry::Remove(ThreadData* thread)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(thread->IsEnqueued());

	int32 priority = thread->GetRunQueueLink()->fPriority;

	thread->SetDequeued();
	fRunQueue.Remove(thread);
	atomic_add(&fThreadCount, -1);

	if (!thread->IsIdle()) {
		Core()->DecrementTotalThreadCount();
		if (priority >= B_DISPLAY_PRIORITY)
			Core()->DecrementDisplayThreadCount();
	}
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

	int32 currentLoad = atomic_get(const_cast<int32*>(&fLoad));
	int oldLoad = compute_load(fMeasureTime, fMeasureActiveTime, currentLoad,
			system_time());
	if (oldLoad < 0)
		return;

	if (currentLoad < 0)
		currentLoad = 0;
	else if (currentLoad > kLoadClampMax)
		currentLoad = kLoadClampMax;

	atomic_set(&fLoad, currentLoad);

	if (GetLoad() > kVeryHighLoad)
		Scheduler::RebalanceIRQs(false);
}


ThreadData*
CPUEntry::ChooseNextThread(ThreadData* oldThread, bool putAtBack)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT_SCHED_LOCK();

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
			cpuLocker.Unlock();
			bool wasRunQueueEmpty;
			bool requestPreemption;
			bool updateInteraction;
			if (!sharedThread->Enqueue(wasRunQueueEmpty, requestPreemption,
					updateInteraction)) {
				Thread* const stolenThread = sharedThread->GetThread();
				if (!enqueue_safe(stolenThread)) {
					sharedThread->MigrateTo(fCore);
					bool dummy1, dummy2;
					sharedThread->Enqueue(dummy1, dummy2, updateInteraction);
				}
			}

			if (updateInteraction)
				scheduler_update_interaction_state();
		}
		return oldThread;
	}

	if (sharedPriority > pinnedPriority) {
		if (sharedThread->fStolen) {
			fCore->DecrementTotalThreadCount();
			sharedThread->fStolen = false;
		}
		if (sharedThread->Core() == fCore && !sharedThreadIsFloating)
			fCore->Remove(sharedThread);
		return sharedThread;
	}

	coreLocker.Unlock();

	if (sharedThreadIsFloating) {
		cpuLocker.Unlock();
		bool wasRunQueueEmpty;
		bool requestPreemption;
		bool updateInteraction;
		if (!sharedThread->Enqueue(wasRunQueueEmpty, requestPreemption,
				updateInteraction)) {
			Thread* const thread = sharedThread->GetThread();
			if (!enqueue_safe(thread)) {
				sharedThread->MigrateTo(fCore);
				bool dummy1, dummy2;
				sharedThread->Enqueue(dummy1, dummy2, updateInteraction);
			}
		}

		if (updateInteraction)
			scheduler_update_interaction_state();
	}

	if (!cpuLocker.IsLocked())
		cpuLocker.Lock();

	if (pinnedThread != NULL && pinnedThread->IsEnqueued()) {
		Remove(pinnedThread);
		return pinnedThread;
	}
	ThreadData* fallback = PeekThread();
	if (fallback != NULL) {
		Remove(fallback);
		return fallback;
	}
	return NULL;
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

		oldThreadData->UpdateActivity(active, system_time());
	}
}


void
CPUEntry::TrackLoad(ThreadData* nextThreadData)
{
	SCHEDULER_ENTER_FUNCTION();

	cpu_ent* cpuEntry = &gCPU[fCPUNumber];

	Thread* nextThread = nextThreadData->GetThread();
	if (!thread_is_idle_thread(nextThread)) {
		cpuEntry->last_kernel_time = nextThread->kernel_time;
		cpuEntry->last_user_time = nextThread->user_time;
		nextThreadData->SetLastInterruptTime(cpuEntry->interrupt_time);
	}

	if (gTrackCPULoad) {
		if (!cpuEntry->disabled)
			ComputeLoad();
		_RequestPerformanceLevel(nextThreadData);
	}
}


uint32
CPUEntry::GetRandom()
{
	uint64 x = fRandomState;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	fRandomState = x;
	return (uint32)((x * 0x2545F4914F6CDD1DULL) >> 32);
}









ThreadData*
CPUEntry::_TryStealWork()
{
	SCHEDULER_ENTER_FUNCTION();

	PackageEntry* package = fCore->Package();

	int32 registeredCores = package->RegisteredCoreCount();
	if (registeredCores <= 1)
		return NULL;

	int32 startIndex = (int32)get_random_index(GetRandom(), registeredCores);

	for (int32 i = 0; i < registeredCores; i++) {
		int32 index = startIndex + i;
		if (index >= registeredCores)
			index -= registeredCores;

		CoreEntry* victim = package->GetCore(index);

		if (victim == NULL || victim == fCore || victim->CPUCount() == 0)
			continue;

		if ((package->IdleCoreMask()
				& ((native_cpu_mask_t)1 << victim->PackageIndex())) != 0) {
			continue;
		}

		if (victim->TryLockRunQueue()) {
			int32 stolenPriority = -1;
			ThreadData* stolen = victim->StealThread(stolenPriority, fCPUNumber);

			if (stolen != NULL) {
				stolen->MigrateTo(fCore);
				stolen->fStolen = true;
				fCore->IncrementTotalThreadCount();
			}

			victim->UnlockRunQueue();

			if (stolen != NULL)
				return stolen;
		}
	}

	ThreadData* stolen = NULL;

	{
	SchedulerNode* node = package->Node();
	if (node == NULL)
		goto phase3;

	search_local_node(node, LocalNodeStealAction(this, package, &stolen));

	if (stolen != NULL)
		return stolen;
	}

phase3:
	search_global_random(GlobalRandomStealAction(this, package, &stolen));

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

	int32 load = max_c(threadData->GetLoad(), GetLoad());

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
	fDisplayThreadCount(0),
	fActiveTime(0),
	fLoad(0),
	fCombinedLoad(0),
	fLastLoadUpdate(0),
	fScoreFactor(1 << 16)
{
	B_INITIALIZE_SPINLOCK(&fCPULock);
	B_INITIALIZE_SPINLOCK(&fQueueLock);
	fLocalIndices = 0;
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
	IncrementTotalThreadCount();
	if (priority >= B_DISPLAY_PRIORITY)
		IncrementDisplayThreadCount();
}


void
CoreEntry::PushBack(ThreadData* thread, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();

	fRunQueue.PushBack(thread, priority);
	atomic_add(&fThreadCount, 1);
	IncrementTotalThreadCount();
	if (priority >= B_DISPLAY_PRIORITY)
		IncrementDisplayThreadCount();
}


void
CoreEntry::Remove(ThreadData* thread)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!thread->IsIdle());
	ASSERT(thread->IsEnqueued());

	int32 priority = thread->GetRunQueueLink()->fPriority;

	thread->SetDequeued();

	atomic_add(&fThreadCount, -1);
	DecrementTotalThreadCount();
	if (priority >= B_DISPLAY_PRIORITY)
		DecrementDisplayThreadCount();
	fRunQueue.Remove(thread);
}




ThreadData*
CoreEntry::StealThread(int32& stolenPriority, int32 thiefCPU)
{
	SCHEDULER_ENTER_FUNCTION();

	CPUSet enabledSnapshot;
	{
		const int32 kWords = (SMP_MAX_CPUS + 31) / 32;
		for (int32 w = 0; w < kWords; w++) {
			int32* ptr = const_cast<int32*>(
				reinterpret_cast<const int32*>(gCPUEnabled.Bits()) + w);
			enabledSnapshot.SetWord(w, (uint32)atomic_get(ptr));
		}
	}

	ThreadData* thread = fRunQueue.PeekOption(StealThreadPredicate(enabledSnapshot, thiefCPU));

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

	// Issue 1/8/9/13/21/59 fix: assign fCoreLocalIndex sequentially
	// before fCPUSet advertisement so concurrent choosers see valid indices.
	native_cpu_mask_t indices;
	int32 bit;
	do {
		indices = scheduler_atomic_get(&fLocalIndices);
		bit = scheduler_ctz(~indices);
		if (bit < 0 || bit >= kMaxCoresPerPackage) {
			panic("CoreEntry::AddCPU: no more core-local indices available (cpu %"
				B_PRId32 ")", cpu->ID());
		}
	} while (scheduler_atomic_test_and_set(&fLocalIndices,
			indices | ((native_cpu_mask_t)1 << bit), indices) != indices);

	cpu->fCoreLocalIndex = bit;

	fCPUSet.SetBitAtomic(cpu->ID());

	bool didAddIdle = false;
	if (firstCPU) {
		didAddIdle = true;
		fLoad = 0;
		atomic_set64(&fCombinedLoad, 0);

		// Issue 67 fix: zero fCoreLoads and fIdleCoreCount explicitly to
		// prevent stale data corruption after topology rebuilds.
		atomic_set(&fPackage->fCoreLoads[fPackageIndex], 0);
		scheduler_atomic_or(&fPackage->fEnabledCoreMask,
			(native_cpu_mask_t)1 << fPackageIndex);

		fPackage->AddIdleCore(this);
	}

	if (fCPUHeap.Insert(cpu, B_IDLE_PRIORITY) != B_OK) {
		fCPUSet.ClearBitAtomic(cpu->ID());
		if (firstCPU) {
			scheduler_atomic_and(&fLocalIndices,
				~((native_cpu_mask_t)1 << bit));
			fLoad = 0;
			atomic_set64(&fCombinedLoad, 0);
			atomic_set(&fPackage->fCoreLoads[fPackageIndex], 0);
			if (didAddIdle)
				fPackage->RemoveIdleCore(this);
			scheduler_atomic_and(&fPackage->fEnabledCoreMask,
				~((native_cpu_mask_t)1 << fPackageIndex));
			atomic_add(&fCPUCount, -1);
		} else {
			atomic_add(&fCPUCount, -1);
			scheduler_atomic_and(&fLocalIndices,
				~((native_cpu_mask_t)1 << bit));
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
	ASSERT(atomic_get(&fIdleCPUCount) >= 0);

	// Issue 96 fix: only call RemoveIdleCore if the core was actually idle
	// (fIdleCPUCount >= 1) to preserve package idle core accounting.
	int32 keyAtRemoval = CPUPriorityHeap::GetKey(cpu);
	bool wasIdle = (keyAtRemoval == B_IDLE_PRIORITY);

	if (wasIdle) {
		// Issue 4/31 fix: core removal iteration limits.
		atomic_add(&fIdleCPUCount, -1);
	}

	fCPUSet.ClearBitAtomic(cpu->ID());

	scheduler_atomic_and(&fLocalIndices,
		~((native_cpu_mask_t)1 << cpu->fCoreLocalIndex));

	int32 oldCPUCount = atomic_add(&fCPUCount, -1);

	if (oldCPUCount == 1) {
		scheduler_atomic_and(&fPackage->fEnabledCoreMask,
			~((native_cpu_mask_t)1 << fPackageIndex));

		if (wasIdle)
			fPackage->RemoveIdleCore(this);

		while (true) {
			ThreadData* threadData;
			{
				// Issue 66 fix: the drain loop uses a per-iteration
				// CoreRunQueueLocker and verifies fThreadCount == 0 post-drain.
				CoreRunQueueLocker locker(this);
				threadData = fRunQueue.PeekMaximum();
				if (threadData == NULL)
					break;

				Remove(threadData);
			}

			ASSERT(threadData->Core() == NULL);
			threadPostProcessing(threadData);
		}

		int32 residual = atomic_get(&fThreadCount);
		if (residual != 0) {
			dprintf("CoreEntry::RemoveCPU: fThreadCount=%" B_PRId32
				" after drain (expected 0) — resetting\n", residual);
			atomic_set(&fThreadCount, 0);
		}
	} else {
		if (!wasIdle && atomic_get(&fIdleCPUCount) == oldCPUCount - 1)
			fPackage->CoreGoesIdle(this);
	}

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
	// Issue 20/27/52 fix: verify the CPU is enabled and present in the
	// priority heap (key >= 0) to prevent returning removed CPUs.
	if (fCPUCount > 1 && GetScore() == 0) {
		const int kWords = (SMP_MAX_CPUS + 31) / 32;
		for (int i = 0; i < kWords; i++) {
			uint32 bits = fCPUSet.Bits(i);
			if (bits == 0)
				continue;
			int cpu = i * 32 + scheduler_ctz((native_cpu_mask_t)bits);
			if (cpu < smp_get_num_cpus()) {
				CPUEntry* entry = &gCPUEntries[cpu];
				if (entry->Core() == this && !gCPU[cpu].disabled
						&& CPUPriorityHeap::GetKey(entry) >= B_IDLE_PRIORITY)
					return entry;
			}
			break;
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
	if (!forceUpdate) {
		if (now < kLoadMeasureInterval + lastUpdate)
			return;
		if (atomic_test_and_set64(&fLastLoadUpdate, now, lastUpdate)
				!= lastUpdate) {
			return;
		}
	} else {
		if (atomic_test_and_set64(&fLastLoadUpdate, now, lastUpdate)
				!= lastUpdate) {
			return;
		}
	}

	int32 currentLoad = 0;
	int64 oldCombined = atomic_get64(&fCombinedLoad);
	int outerRetryCount = 0;
	while (true) {
		currentLoad = (int32)(oldCombined >> 32);
		uint32 nextEpoch = (uint32)oldCombined + 1;
		int64 newCombined = (int64)nextEpoch;

		int64 actual = atomic_test_and_set64(&fCombinedLoad, newCombined,
			oldCombined);
		if (actual == oldCombined) {
			int32 prevLoad = atomic_get(&fLoad);
			int32 currentFLoad = prevLoad;

			static const int kMaxFLoadRetries = 32;
			int innerRetryCount = 0;
			while (true) {
				int32 delta = currentLoad - prevLoad;
				int32 newFLoad = currentFLoad + delta;
				if (newFLoad < 0)
					newFLoad = 0;

				int32 actual = atomic_test_and_set(&fLoad, newFLoad,
					currentFLoad);
				if (actual == currentFLoad)
					break;

				currentFLoad = actual;
				if (++innerRetryCount >= kMaxFLoadRetries) {
					atomic_add(&fLoad, delta);
					break;
				}
			}
			break;
		}

		static const int kMaxCombinedRetries = 64;
		if (++outerRetryCount >= kMaxCombinedRetries) {
			int32 freshCPUCount = atomic_get((int32*)&fCPUCount);
			if (freshCPUCount <= 0) return;
			if (cpuCount > 0) {
				int32 load = (int32)(oldCombined >> 32) / freshCPUCount;
				load = ((int64)load * fScoreFactor) >> 16;
				atomic_set(&fPackage->fCoreLoads[fPackageIndex],
					min_c(load, (int32)kMaxLoad));
			}
			return;
		}
		oldCombined = actual;
	}

	if (cpuCount > 0) {
		int32 load = currentLoad / cpuCount;
		load = ((int64)load * fScoreFactor) >> 16;

		int32 oldLoad = atomic_get(&fPackage->fCoreLoads[fPackageIndex]);
		atomic_set(&fPackage->fCoreLoads[fPackageIndex],
			SmoothLoad(oldLoad, min_c(load, (int32)kMaxLoad)));
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
	fMaxAttempts = 0;
	memset(fCores, 0, sizeof(fCores));
	memset(fCoreLoads, 0, sizeof(fCoreLoads));
	fIdleCoreCount = 0;
}


void
PackageEntry::AddIdleCore(CoreEntry* core)
{
	WriteSpinLocker coreLocker(fCoreLock);
	native_cpu_mask_t oldMask = scheduler_atomic_or(&fIdleCoreMask,
		(native_cpu_mask_t)1 << core->PackageIndex());
	atomic_add(&fIdleCoreCount, 1);

	if (oldMask == 0) {
		if (fNode != NULL)
			fNode->PackageGoesIdle(this);
	}
}


void
PackageEntry::RemoveIdleCore(CoreEntry* core)
{
	WriteSpinLocker coreLocker(fCoreLock);
	native_cpu_mask_t clearBit = (native_cpu_mask_t)1 << core->PackageIndex();
	native_cpu_mask_t oldMask = scheduler_atomic_and(&fIdleCoreMask, ~clearBit);

	atomic_add(&fIdleCoreCount, -1);

	if ((oldMask & ~clearBit) == 0) {
		if (fNode != NULL)
			fNode->PackageWakesUp(this);
	}
}


CoreEntry*
PackageEntry::GetIdleCore(int32 index) const
{
	native_cpu_mask_t mask = scheduler_atomic_get(&fIdleCoreMask);
	if (mask == 0)
		return NULL;

	native_cpu_mask_t currentMask = mask;

	for (int32 i = 0; i < index; i++) {
		int32 bit = scheduler_ctz(currentMask);
		currentMask &= ~((native_cpu_mask_t)1 << bit);

		if (currentMask == 0) {
			int32 fallbackBit = scheduler_ctz(mask);
			if (fallbackBit >= 0 && fallbackBit < kMaxCoresPerPackage)
				return fCores[fallbackBit];
			return NULL;
		}
	}

	int32 finalBit = scheduler_ctz(currentMask);
	if (finalBit >= 0 && finalBit < kMaxCoresPerPackage)
		return fCores[finalBit];
	return NULL;
}


CoreEntry*
PackageEntry::GetIdleCorePacking(CPUEntry* cpu, const CPUSet* affinity) const
{
	native_cpu_mask_t mask = scheduler_atomic_get(&fIdleCoreMask);
	if (mask == 0)
		return NULL;

	// Issue 71/98 fix: search_local_node uses 64-bit bitmasks for deduplication
	// and respects affinity masks during multi-core/package scans.
	native_cpu_mask_t enabledMask = scheduler_atomic_get((native_cpu_mask_t*)&fEnabledCoreMask);
	native_cpu_mask_t activeMask = enabledMask & ~mask;

	if (activeMask != 0) {
		native_cpu_mask_t neighbors = ((activeMask << 1) | (activeMask >> 1)) & mask;
		if (neighbors != 0) {
			if (scheduler_popcount(neighbors) > 1) {
				int32 shift = 1 + (int32)(((uint64)cpu->GetRandom()
					* (uint64)(kMaxCoresPerPackage - 1)) >> 32);
				if (shift >= (int32)kMaxCoresPerPackage) {
					return fCores[scheduler_ctz(neighbors)];
				}
				native_cpu_mask_t rotated = (neighbors >> shift)
					| (neighbors << (kMaxCoresPerPackage - shift));

				if (rotated != 0) {
					int32 pos = scheduler_ctz(rotated);
					int32 origIdx = (pos + shift) % kMaxCoresPerPackage;
					if (origIdx >= 0 && origIdx < kMaxCoresPerPackage
							&& fCores[origIdx] != NULL
							&& (neighbors & ((native_cpu_mask_t)1 << origIdx))) {
						if (affinity == NULL
								|| fCores[origIdx]->CPUMask().Matches(*affinity)) {
							return fCores[origIdx];
						}
					}
				}
			}

			native_cpu_mask_t candidateMask = neighbors;
			while (candidateMask != 0) {
				int32 bit = scheduler_ctz(candidateMask);
				if (fCores[bit] != NULL && (affinity == NULL
						|| fCores[bit]->CPUMask().Matches(*affinity))) {
					return fCores[bit];
				}
				candidateMask &= ~((native_cpu_mask_t)1 << bit);
			}
		}
	}

	int32 count = scheduler_popcount(mask);
	if (count > 1) {
		int32 startIndex = (int32)get_random_index(cpu->GetRandom(), count);
		for (int32 i = 0; i < count; i++) {
			int32 index = (startIndex + i) % count;
			CoreEntry* candidate = GetIdleCore(index);
			if (candidate != NULL && (affinity == NULL
					|| candidate->CPUMask().Matches(*affinity))) {
				return candidate;
			}
		}
	}

	int32 bit = scheduler_ctz(mask);
	if (bit >= 0 && bit < kMaxCoresPerPackage && fCores[bit] != NULL
			&& (affinity == NULL
				|| fCores[bit]->CPUMask().Matches(*affinity))) {
		return fCores[bit];
	}
	if (affinity != NULL) {
		native_cpu_mask_t remaining = mask;
		if (bit >= 0)
			remaining &= ~((native_cpu_mask_t)1 << bit);
		while (remaining != 0) {
			int32 nextBit = scheduler_ctz(remaining);
			remaining &= ~((native_cpu_mask_t)1 << nextBit);
			if (nextBit >= 0 && nextBit < kMaxCoresPerPackage
					&& fCores[nextBit] != NULL
					&& fCores[nextBit]->CPUMask().Matches(*affinity)) {
				return fCores[nextBit];
			}
		}
	}
	return NULL;
}


void
PackageEntry::RegisterCore(int32 index, CoreEntry* core)
{
	// Issue 86 fix: production guard against out-of-bounds writes on
	// release builds.
	if (index < 0 || index >= kMaxCoresPerPackage) {
		dprintf("PackageEntry::RegisterCore: index %" B_PRId32 " out of range"
			" [0, %" B_PRId32 ") — core registration skipped\n",
			index, (int32)kMaxCoresPerPackage);
		return;
	}
	fCores[index] = core;
	fRegisteredCoreCount = max_c(fRegisteredCoreCount, index + 1);
	fCoreCount++;

	if (fRegisteredCoreCount > 0) {
		fMaxAttempts = 4 + (3 * (fls((uint32)fRegisteredCoreCount) - 1)) / 2;
	} else
		fMaxAttempts = 0;
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

	if (fRegisteredCoreCount > kRandomCoreSearchThreshold) {
		uint64 sampledCores = 0;
		int32 attempts = 0;
		int32 registeredCores = fRegisteredCoreCount;
		if (registeredCores <= 0)
			return NULL;

		while (attempts++ < fMaxAttempts) {
			int32 i = (int32)get_random_index(cpu->GetRandom(), registeredCores);

			if (sampledCores & (1ULL << i))
				continue;
			sampledCores |= (1ULL << i);

			CoreEntry* candidate = fCores[i];
			if (candidate == NULL)
				continue;

			if (!(((native_cpu_mask_t)1 << i) & enabledMask))
				continue;

			if (mask != NULL && !candidate->CPUMask().Matches(*mask))
				continue;
			if (type != CORE_TYPE_UNKNOWN && candidate->Type() != type)
				continue;

			int32 load = atomic_get(&fCoreLoads[i]);

			if (minEntry == NULL || load < minLoad) {
				minLoad = load;
				minEntry = candidate;
			}
		}

		if (minEntry != NULL)
			return minEntry;
	}

	native_cpu_mask_t currentEnabledMask = enabledMask;
	while (currentEnabledMask != 0) {
		int32 i = scheduler_ctz(currentEnabledMask);
		currentEnabledMask &= ~((native_cpu_mask_t)1 << i);

		CoreEntry* candidate = fCores[i];
		if (candidate == NULL)
			continue;
		if (mask != NULL && !candidate->CPUMask().Matches(*mask))
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

	if (fRegisteredCoreCount > kRandomCoreSearchThreshold) {
		uint64 sampledCores = 0;
		int32 attempts = 0;
		int32 registeredCores = fRegisteredCoreCount;
		if (registeredCores <= 0)
			return NULL;

		while (attempts++ < fMaxAttempts) {
			int32 i = (int32)get_random_index(cpu->GetRandom(), registeredCores);

			if (sampledCores & (1ULL << i))
				continue;
			sampledCores |= (1ULL << i);

			CoreEntry* candidate = fCores[i];
			if (candidate == NULL)
				continue;

			if (!(((native_cpu_mask_t)1 << i) & enabledMask))
				continue;

			if (mask != NULL && !candidate->CPUMask().Matches(*mask))
				continue;
			if (type != CORE_TYPE_UNKNOWN && candidate->Type() != type)
				continue;

			int32 load = atomic_get(&fCoreLoads[i]);

			if (maxEntry == NULL || load > maxLoad
					|| (load == maxLoad
						&& candidate->PackageIndex() > maxEntry->PackageIndex())) {
				maxLoad = load;
				maxEntry = candidate;
			}
		}

		if (maxEntry != NULL)
			return maxEntry;
	}

	int32 count = scheduler_popcount(enabledMask);
	int32 startBit = 0;

	if (count > 1) {
		startBit = (int32)get_random_index(cpu->GetRandom(),
			fRegisteredCoreCount);
	}

	native_cpu_mask_t upperMask = enabledMask & (~(native_cpu_mask_t)0 << startBit);
	native_cpu_mask_t lowerMask = enabledMask & (((native_cpu_mask_t)1 << startBit) - 1);

	if (upperMask == 0) {
		upperMask = lowerMask;
		lowerMask = 0;
	}

	for (int pass = 0; pass < 2; pass++) {
		native_cpu_mask_t currentMask = (pass == 0) ? upperMask : lowerMask;
		if (currentMask == 0)
			break;

		while (currentMask != 0) {
			int32 i = scheduler_ctz(currentMask);
			currentMask &= ~((native_cpu_mask_t)1 << i);

			CoreEntry* candidate = fCores[i];
			if (candidate == NULL)
				continue;
			if (mask != NULL && !candidate->CPUMask().Matches(*mask))
				continue;
			if (type != CORE_TYPE_UNKNOWN && candidate->Type() != type)
				continue;

			int32 load = atomic_get(&fCoreLoads[i]);
			if (maxEntry == NULL || load > maxLoad
					|| (load == maxLoad
						&& candidate->PackageIndex() > maxEntry->PackageIndex())) {
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
	kprintf("%13" B_PRId32 " ", package->fPackageID);

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
			int32 nodeIndex = scheduler_ffs64(nodeMask) - 1;
			nodeMask &= ~(1ULL << nodeIndex);

			uint64 packageMask = gSchedulerNodes[nodeIndex].IdlePackageMask();
			while (packageMask != 0) {
				int32 packageIndex = scheduler_ffs64(packageMask) - 1;
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

}	// namespace Scheduler
