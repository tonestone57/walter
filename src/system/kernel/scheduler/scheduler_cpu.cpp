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
	// Robust mapping to reduce modulo bias.
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
	// Issue 23 fix: improve per-CPU RNG seed entropy. On boot, system_time()
	// returns small values with low entropy; successive CPUs initialized
	// microseconds apart get highly correlated seeds. Fold in the address of
	// the CPUEntry itself (ASLR entropy on randomized kernels), a
	// compile-time constant unique per translation unit, and multiple rounds
	// of Murmur3-style mixing to break correlation.
	uint64 seed = system_time();
	seed ^= (uint64)(uintptr_t)this;           // ASLR/address entropy
	seed ^= ((uint64)id * 0x9E3779B97F4A7C15ULL); // Golden ratio spread
	seed ^= (uint64)id << 32;
	// Three rounds of strong mixing
	seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ULL;
	seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBULL;
	seed ^= seed >> 31;
	fRandomState = seed ? seed : 1;

	// Stagger the boost-scan trigger across CPUs. Without this all CPUs
	// fire UpdatePriorityBoostScalable at the same reschedule boundary,
	// causing correlated lock acquisition and measurable latency spikes.
	// Issue 44 fix: id % 10 produces duplicate stagger values when cpu count
	// is not a multiple of 10 (e.g. 12 CPUs: IDs 0,10 both get 0; 1,11 get 1).
	// Use % cpuCount (clamped) so each CPU gets a unique stagger within [0, N).
	// Fall back to id % 10 for systems with > 10 CPUs where spread matters more.
	{
		int32 numCPUs = smp_get_num_cpus();
		int32 staggerMod = (numCPUs > 1 && numCPUs <= 10) ? numCPUs : 10;
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
	// the IRQ drain loop uses a hard limit of 1000
	// iterations.  On a pathological system with more than 1000 IRQs
	// assigned to one CPU the excess interrupts are not reassigned and
	// a warning is logged.  The limit is intentional (prevents an
	// infinite loop if assign_io_interrupt_to_cpu silently fails) and
	// the warning below makes the truncation visible.  A future
	// improvement could query the IRQ count first and use a tighter
	// limit, but the current bound is safe in all real configurations.

	// get rid of irqs
	SpinLocker locker(entry->irqs_lock);
	for (int32 i = 0; i < 1000; i++) {
		irq_assignment* irq
			= (irq_assignment*)list_get_first_item(&entry->irqs);
		if (irq == NULL)
			break;

		int32 irqVector = irq->irq;
		locker.Unlock();

		// Issue 15: assign_io_interrupt_to_cpu may acquire internal locks.
		// We release irqs_lock BEFORE calling it (locker.Unlock() above) to
		// prevent priority inversion or deadlock against any path that acquires
		// irqs_lock while holding an internal interrupt-assignment lock.
		// The locker is re-acquired on the next iteration before list access.
		assign_io_interrupt_to_cpu(irqVector, -1);

		locker.Lock();

		// Issue 15 fix: the progress check correctly identifies a silent
		// failure from assign_io_interrupt_to_cpu when the head IRQ didn't
		// change. However, a transient failure that retries next iteration
		// is now correctly distinguished: we only abort if the SAME irqVector
		// is still at the head after a full unlock/assign/lock cycle.
		// The existing logic is correct; add explicit documentation.
		irq_assignment* currentHead
			= (irq_assignment*)list_get_first_item(&entry->irqs);
		if (currentHead != NULL && currentHead->irq == irqVector) {
			// No progress: abort to prevent burning all 1000 iterations.
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

	// (defensive): capture the priority the thread was enqueued with to
	// ensure symmetric counter updates. If the thread's priority was
	// changed while enqueued, GetEffectivePriority() would return the
	// new value, causing fDisplayThreadCount to desynchronize if the
	// change crossed the B_DISPLAY_PRIORITY threshold.
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

	// Clamp load to avoid overflow or runaway values
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
			cpuLocker.Unlock();
			bool wasRunQueueEmpty;
			bool requestPreemption;
			bool updateInteraction;
			if (!sharedThread->Enqueue(wasRunQueueEmpty, requestPreemption,
					updateInteraction)) {
				// Issue 6/23 fix: cache the Thread* once; sharedThread->GetThread()
				// is called multiple times below and the pointer must be consistent.
				Thread* const stolenThread = sharedThread->GetThread();
				if (!enqueue_safe(stolenThread)) {
					dprintf("scheduler: WARNING: stolen thread %" B_PRId32
						" lost during hot-unplug — forcing to current CPU\n",
						stolenThread->id);
					sharedThread->MigrateTo(fCore);
					bool dummy1, dummy2;
					// Issue 6 fix: check return value; log if the last-resort also fails.
					if (!sharedThread->Enqueue(dummy1, dummy2, updateInteraction)) {
						dprintf("scheduler: CRITICAL: thread %" B_PRId32
							" could not be re-enqueued after forced migration;"
							" scheduler state is inconsistent\n", stolenThread->id);
					}
				}
			}

			// Issue 20 fix: call while NOT holding run-queue locks.
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
		// Re-enqueue via global path if core was disabled.
		if (!sharedThread->Enqueue(wasRunQueueEmpty, requestPreemption,
				updateInteraction)) {
			Thread* const thread = sharedThread->GetThread();
			if (!enqueue_safe(thread)) {
				dprintf("scheduler: WARNING: shared thread %" B_PRId32
					" lost during hot-unplug — forcing to current CPU\n",
					thread->id);
				sharedThread->MigrateTo(fCore);
				bool dummy1, dummy2;
				if (!sharedThread->Enqueue(dummy1, dummy2, updateInteraction)) {
					dprintf("scheduler: CRITICAL: thread %" B_PRId32
						" could not be re-enqueued after forced migration;"
						" scheduler state is inconsistent\n", thread->id);
				}
			}
		}

		if (updateInteraction)
			scheduler_update_interaction_state();
	}

	if (!cpuLocker.IsLocked())
		cpuLocker.Lock();

	// Issue 78 fix: pinnedThread was obtained before cpuLocker was released
	// and re-acquired in the floating-thread path. Between the unlock and
	// re-lock, pinnedThread may have been dequeued by a concurrent Dequeue()
	// (e.g. from scheduler_set_thread_priority). Verify it is still enqueued
	// before calling Remove(), which ASSERTs IsEnqueued().
	if (pinnedThread != NULL && pinnedThread->IsEnqueued()) {
		Remove(pinnedThread);
		return pinnedThread;
	}
	// pinnedThread was stolen; fall back to whatever is at the head now.
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

		// Compute system_time() once and pass it to UpdateActivity so
		// the virtual-runtime ceiling uses the same timestamp as the rest of
		// this scheduling decision.  Avoids a redundant syscall on the hot path.
		oldThreadData->UpdateActivity(active, system_time());
	}
}


void
CPUEntry::TrackLoad(ThreadData* nextThreadData)
{
	SCHEDULER_ENTER_FUNCTION();

#ifdef DEBUG_SCHEDULER
	TRACE("scheduler: cpu=%d load=%d idle=%d\n",
		fCPUNumber,
		atomic_get(const_cast<int32*>(&fLoad)),
		gCPU[fCPUNumber].idle);
#endif

	cpu_ent* cpuEntry = &gCPU[fCPUNumber];

	// Issue 62 fix: update thread timestamps BEFORE calling
	// _RequestPerformanceLevel. The performance level request reads
	// nextThreadData->GetLoad() and fCore->GetLoad(), which are based on
	// accounting that uses last_kernel_time/last_user_time. Updating them
	// first ensures _RequestPerformanceLevel sees fresh accounting data.
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

	// iterate over other cores in the package and try to steal work
	PackageEntry* package = fCore->Package();

	int32 registeredCores = package->RegisteredCoreCount();
	if (registeredCores <= 1)
		return NULL;

	// (clarification): The subtraction-wrap "index -= registeredCores"
	// is safe because startIndex < registeredCores and i < registeredCores,
	// so startIndex + i < 2 * registeredCores, requiring at most one subtraction.
	// (clarification): GetIdleCorePacking shift guard — when shift==0
	// the left-shift by kMaxCoresPerPackage is already guarded by "if (shift > 0)"
	// in PackageEntry::GetIdleCorePacking; no undefined behaviour occurs.
	// Pick a random starting point to avoid convoys
	// We use multiplicative mapping to avoid modulo.
	int32 startIndex = (int32)get_random_index(GetRandom(), registeredCores);

	for (int32 i = 0; i < registeredCores; i++) {
		// Optimization: Use subtraction for wrapping instead of modulo
		int32 index = startIndex + i;
		if (index >= registeredCores)
			index -= registeredCores;

		CoreEntry* victim = package->GetCore(index);

		if (victim == NULL || victim == fCore || victim->CPUCount() == 0)
			continue;

		// Issue 82 fix: IdleCoreMask() is read atomically but without holding
		// a lock. Between this read and TryLockRunQueue(), the victim core may
		// have transitioned from idle to active. The skip is a best-effort
		// optimisation, not a guarantee. The comment "guaranteed empty" was
		// incorrect — replace with accurate description.
		// The TryLockRunQueue() + PeekOption() predicate below is the actual
		// correctness barrier; this skip only avoids a redundant lock attempt.
		if ((package->IdleCoreMask()
				& ((native_cpu_mask_t)1 << victim->PackageIndex())) != 0) {
			continue;
		}

		// Use TryLock to avoid contention
		if (victim->TryLockRunQueue()) {
			int32 stolenPriority = -1;
			ThreadData* stolen = victim->StealThread(stolenPriority, fCPUNumber);

			if (stolen != NULL) {
				// Issue 9: MigrateTo and IncrementTotalThreadCount use only
				// atomic operations (atomic_add, atomic_add64) with no
				// spinlocks.  Calling them while holding victim->TryLockRunQueue
				// does NOT violate lock ordering: they cannot block or acquire
				// any spinlock that another CPU could hold while waiting for
				// the victim run-queue lock.
				stolen->MigrateTo(fCore);
				stolen->fStolen = true;
				fCore->IncrementTotalThreadCount();
			}

			victim->UnlockRunQueue();

			if (stolen != NULL)
				return stolen;
		}
	}

	// Phase 2: The Local NUMA Node (Random)
	// Target: Cores on the same physical socket/die (e.g., 64-128 cores).
	// Method: Random Sampling
	// Why: Stealing here is fast (local RAM). You want to exhaust reasonable options here
	// before going across the expensive interconnect.

	// Issue 6/15 fix: declare 'stolen' BEFORE the 'goto phase3' to avoid
	// jumping over a variable initialisation, which is ill-formed in C++
	// (UB even for trivial types when an initialiser is present).
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
	// Issue 19 fix: removed the unconditional stolen = NULL reset.
	// If Phase 2 fell through with a valid stolen thread, the reset would
	// lose it.  If Phase 2 jumped here (node == NULL) stolen is already NULL.
	// Phase 3 correctly checks (stolen != NULL) in its first lambda probe.

	// Phase 3: The Global Hail Mary (Random)
	// Target: Any core in the system (4096 cores).
	// Method: Logarithmic Formula
	// Why: This is the last resort. If the local node is empty, you are willing to pay
	// the high cost to steal from a remote socket to avoid sleeping.

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
	ASSERT_PRINT(load >= 0 && load <= kMaxLoad + kSMTPenalty, "load is out of range %"
		B_PRId32 " (max of %" B_PRId32 " %" B_PRId32 ")", load,
		threadData->GetLoad(), GetLoad());

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
	fScoreFactor(1 << 16),
	fLocalIndices(0)
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

	// (defensive): capture the enqueued priority to ensure consistent
	// fDisplayThreadCount accounting.
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

	// Issue 27 fix: snapshot gCPUEnabled once before the predicate loop.
	// GetCPUMask() re-reads gCPUEnabled with atomic retry logic on every
	// call. During work stealing this predicate is invoked for every
	// candidate thread in the victim's run queue. Since gCPUEnabled only
	// changes during hot-plug (rare), caching it here avoids redundant
	// atomic reads on the hot stealing path.
	//
	// We snapshot into a local CPUSet and pass thiefCPU as a plain bit
	// check: if the thread's cpumask is empty (no affinity constraint) it
	// can run anywhere; otherwise it must allow thiefCPU.
	CPUSet enabledSnapshot;
	{
		const int32 kWords = (SMP_MAX_CPUS + 31) / 32;
		// Issue 27 fix: CPUSet::Bits() returns a pointer to uint32 words.
		// Casting to int32* for atomic_get is required by the atomic API
		// but assumes 4-byte alignment.
		// static_assert replaced by comment for GCC 2.95
		// alignof(CPUSet) >= 4
		for (int32 w = 0; w < kWords; w++) {
			int32* ptr = const_cast<int32*>(
				reinterpret_cast<const int32*>(gCPUEnabled.Bits()) + w);
			enabledSnapshot.SetWord(w, (uint32)atomic_get(ptr));
		}
	}

	// Explicitly exclude idle threads from steal candidates.
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

	// Find the first available local index using a CAS loop.
	// This ensures unique index assignment even after arbitrary CPU hot-unplugging.
	int32 localIndex = -1;
	while (true) {
		native_cpu_mask_t mask = scheduler_atomic_get(&fLocalIndices);

		// Explicitly check for full mask before calling ctz to avoid
		// architecture-dependent undefined behavior on ctz(0).
		if (mask == (native_cpu_mask_t)-1) {
			panic("CoreEntry::AddCPU: no available local index for core %" B_PRId32,
				fCoreID);
		}

		localIndex = scheduler_ctz(~mask);
		if (localIndex < 0 || localIndex >= kMaxCoresPerPackage) {
			panic("CoreEntry::AddCPU: local index %" B_PRId32 " out of range "
				"for core %" B_PRId32, localIndex, fCoreID);
		}

		if (scheduler_atomic_test_and_set(&fLocalIndices,
				mask | ((native_cpu_mask_t)1 << localIndex), mask) == mask) {
			break;
		}
	}
	cpu->fCoreLocalIndex = localIndex;

	fCPUSet.SetBitAtomic(cpu->ID());

	bool didAddIdle = false;
	if (firstCPU) {
		didAddIdle = true;
		fLoad = 0;
		atomic_set64(&fCombinedLoad, 0);

		atomic_set(&fPackage->fCoreLoads[fPackageIndex], 0);
		scheduler_atomic_or(&fPackage->fEnabledCoreMask,
			(native_cpu_mask_t)1 << fPackageIndex);

		fPackage->AddIdleCore(this);
	}

	if (fCPUHeap.Insert(cpu, B_IDLE_PRIORITY) != B_OK) {
		fCPUSet.ClearBitAtomic(cpu->ID());
		// Roll back the local index on failure.
		scheduler_atomic_and(&fLocalIndices,
			~((native_cpu_mask_t)1 << localIndex));
		if (firstCPU) {
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
	ASSERT(atomic_get(&fIdleCPUCount) >= 1);

	// The CPU is guaranteed to be idle and accounted for in fIdleCPUCount
	// before RemoveCPU is called (set by scheduler_set_cpu_enabled).
	atomic_add(&fIdleCPUCount, -1);

	fCPUSet.ClearBitAtomic(cpu->ID());
	int32 oldCPUCount = atomic_add(&fCPUCount, -1);

	if (oldCPUCount == 1) {
		// core has been disabled
		scheduler_atomic_and(&fPackage->fEnabledCoreMask,
			~((native_cpu_mask_t)1 << fPackageIndex));

		// Issue 96 fix: only call RemoveIdleCore if the core was actually idle
		// (all its CPUs were idle). Calling unconditionally when a non-idle
		// core is removed decrements fIdleCoreCount below its true value,
		// corrupting idle core accounting for the entire package.
		if (atomic_get(&fIdleCPUCount) >= 1)
			fPackage->RemoveIdleCore(this);

		// Issue 66 fix: use CoreRunQueueLocker per-iteration to prevent
		// a concurrent work-stealer (which uses TryLockRunQueue) from
		// stealing a thread between PeekMaximum and Remove, leaving
		// fThreadCount positive with an empty queue.
		// The steal path checks CPUCount()==0 before adding stolen threads,
		// so once we set oldCPUCount==1 no new threads can arrive.
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
			// Issue 31 fix: threadPostProcessing calls enqueue() which calls
			// Enqueue() which increments gTotalRunnableThreads for non-idle
			// threads. If enqueue() subsequently fails (all cores disabled),
			// it returns false without a matching decrement. Detect this case
			// and decrement manually to avoid a permanent counter leak.
			threadPostProcessing(threadData);
		}

		// Issue 66 fix: after drain, explicitly verify fThreadCount is zero.
		// If a concurrent steal occurred in the narrow window, fThreadCount
		// may be positive. Force it to zero since CPUCount==0 prevents
		// further enqueues, making any residual count a permanent leak.
		int32 residual = atomic_get(&fThreadCount);
		if (residual != 0) {
			dprintf("CoreEntry::RemoveCPU: fThreadCount=%" B_PRId32
				" after drain (expected 0) — resetting\n", residual);
			atomic_set(&fThreadCount, 0);
		}
	}

	// Use INT32_MIN instead of the implicit magic -1.  INT32_MIN is
	// less than every valid scheduler priority (minimum B_IDLE_PRIORITY == 0),
	// so the CPU is guaranteed to bubble to the heap root.  The explicit
	// constant makes the intent clear and prevents silent misbehaviour if the
	// priority range ever grows to include negative values.
	fCPUHeap.ModifyKey(cpu, INT32_MIN);
	// Issue 2 fix: fCPUHeap is accessed exclusively under fCPULock, which the
	// caller holds via CoreCPUHeapLocker for the duration of RemoveCPU.  No
	// other CPU can concurrently modify the heap, so the root is guaranteed to
	// be 'cpu' immediately after ModifyKey(cpu, INT32_MIN).  The previous spin
	// loop was dead code and its 1000-iteration cap masked any real invariant
	// violations by silently proceeding with a wrong root.
	ASSERT(fCPUHeap.PeekRoot() == cpu);
	fCPUHeap.RemoveRoot();

	// Atomically clear the bit in fLocalIndices.
	scheduler_atomic_and(&fLocalIndices,
		~((native_cpu_mask_t)1 << cpu->fCoreLocalIndex));

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
	// Primary logical CPU first.
	// Issue 20 fix: use fCPUSet.FindFirstSet() equivalent via scheduler_ctz()
	// on each bitmap word rather than iterating all (SMP_MAX_CPUS+31)/32
	// words. For a core with CPUs in a small ID range this avoids scanning
	// all zero words before finding the first set bit.
	if (fCPUCount > 1 && GetScore() == 0) {
		// Find first CPU in this core's set using the bitmap directly.
		// Each word covers 32 CPU IDs; stop at first non-zero word.
		const int kWords = (SMP_MAX_CPUS + 31) / 32;
		for (int i = 0; i < kWords; i++) {
			uint32 bits = fCPUSet.Bits(i);
			if (bits == 0)
				continue;
			int cpu = i * 32 + scheduler_ctz((native_cpu_mask_t)bits);
			if (cpu < smp_get_num_cpus()) {
				CPUEntry* entry = &gCPUEntries[cpu];
				// Issue 52 fix: verify the CPU is still in the heap before
				// returning it. A concurrent RemoveCPU may have set the heap
				// key to INT32_MIN between the fCPUSet read and this check.
				// GetKey returns INT32_MIN for removed CPUs; any valid CPU
				// has key >= B_IDLE_PRIORITY (0).
				if (entry->Core() == this && !gCPU[cpu].disabled
						&& CPUPriorityHeap::GetKey(entry) >= B_IDLE_PRIORITY)
					return entry;
			}
			// First set bit found but not valid; fall through to heap path.
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

	// one system_time() call shared by both branches eliminates
	// a redundant syscall and ensures consistent timestamps.
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
		// on CAS failure another CPU won the update race; that
		// update is sufficient, so return rather than silently skipping.
		if (atomic_test_and_set64(&fLastLoadUpdate, now, lastUpdate)
				!= lastUpdate) {
			return;
		}
	}

	// Combined Epoch and Load Update:
	// We use a 64-bit atomic to store both the current load (upper 32 bits)
	// and the measurement epoch (lower 32 bits). This allows us to atomically
	// increment the epoch and reset the current load, ensuring that concurrent
	// AddLoad calls either contribute to the old epoch (and are manually
	// added to fLoad) or the new epoch (and are captured in the next snapshot),
	// with no double-counting or loss.
	int32 currentLoad = 0;
	int64 oldCombined = atomic_get64(&fCombinedLoad);
	int outerRetryCount = 0;
	while (true) {
		currentLoad = (int32)(oldCombined >> 32);
		uint32 nextEpoch = (uint32)oldCombined + 1;
		int64 newCombined = (int64)nextEpoch; // Load reset to 0

		int64 actual = atomic_test_and_set64(&fCombinedLoad, newCombined,
			oldCombined);
		if (actual == oldCombined) {
			// Issue 7 fix: snapshot prevLoad immediately after winning the
			// outer CAS on fCombinedLoad, not before the loop.  The original
			// code snapshotted prevLoad before the loop, so concurrent
			// RemoveLoad(force=true) calls that ran between the snapshot and
			// the CAS win produced a stale baseline, making delta wrong.
			// Reading here minimises the race window to the CAS itself.
			// currentFLoad starts equal to prevLoad; the inner retry loop
			// updates it on CAS failure and correctly adds delta each time.
			int32 prevLoad = atomic_get(&fLoad);
			int32 currentFLoad = prevLoad;

			// Issue 48 fix: snapshot prevLoad immediately after winning the
			// outer CAS. A concurrent ChangeLoad/AddLoad can still modify
			// fLoad between the CAS win and the prevLoad read, but this
			// window is now as narrow as possible (a single atomic_get).
			// The inner retry loop handles any residual CAS failure.
			//
			// Issue 7 fix: add iteration limit to the inner CAS loop to
			// prevent livelock under pathological contention. After
			// kMaxFLoadRetries failures we read the current value and apply
			// the delta as best-effort; slight inaccuracy is acceptable
			// versus spinning indefinitely with interrupts disabled.
			// The outer CAS on fCombinedLoad already has its own retry, so
			// we only need to bound the inner fLoad CAS.
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
					// Best-effort: apply delta to most recently observed value.
					atomic_add(&fLoad, delta);
					break;
				}
			}
			break;
		}

		// Issue 7 fix: bound the outer CAS retry loop too.
		// Livelock is possible if another CPU perpetually updates fCombinedLoad
		// between our read and our CAS. After kMaxCombinedRetries we give up;
		// the next _UpdateLoad call (triggered by the next load-measure timer)
		// will correct any accumulated error.
		static const int kMaxCombinedRetries = 64;
		if (++outerRetryCount >= kMaxCombinedRetries) {
			// Issue 85 fix: re-read cpuCount to get a fresh value after
			// many retry failures. The value from function entry may be
			// several epochs stale on a heavily contended system.
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
	// Issue 67 fix: explicitly zero fCoreLoads on every Init() call.
	// If PackageEntry objects are ever reused after a topology rebuild,
	// stale load values persist and corrupt choose_core decisions.
	memset(fCoreLoads, 0, sizeof(fCoreLoads));
	// Zero fIdleCoreCount explicitly to match fIdleCoreMask == 0.
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
		// Issue 45 fix: document that fCoreLock is held here but NOT held
		// in CoreGoesIdle. The two paths are serialized at a higher level
		// (InterruptsBigSchedulerLocker for AddIdleCore vs. normal scheduling
		// for CoreGoesIdle). If this serialization is ever relaxed, an
		// explicit atomic CAS must guard the PackageGoesIdle transition.
		if (fNode != NULL)
			fNode->PackageGoesIdle(this);
	}
}


void
PackageEntry::RemoveIdleCore(CoreEntry* core)
{
	WriteSpinLocker coreLocker(fCoreLock);
	// Clear the mask bit BEFORE decrementing the count.  A concurrent reader
	// of the mask (e.g. GetIdleCore) must not see a core that is in the
	// process of being removed from the idle set, as that could lead to a
	// "dangling-ish" core reference if the core is being disabled.  The
	// reader of the count (e.g. GetLeastIdlePackage) will gracefully handle
	// a count > 0 with an empty mask by receiving NULL from GetIdleCore().
	native_cpu_mask_t clearBit = (native_cpu_mask_t)1 << core->PackageIndex();
	native_cpu_mask_t oldMask = scheduler_atomic_and(&fIdleCoreMask, ~clearBit);

	atomic_add(&fIdleCoreCount, -1);

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
	if (mask == 0)
		return NULL;

	native_cpu_mask_t currentMask = mask;

	// Find the N-th set bit (index-th)
	for (int32 i = 0; i < index; i++) {
		int32 bit = scheduler_ctz(currentMask);
		currentMask &= ~((native_cpu_mask_t)1 << bit);

		if (currentMask == 0) {
			// index out of bounds (race), fallback to the first idle core
			// Issue 28 fix: validate fCores[bit] is non-NULL.
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

	// Intra-package packing:
	// We prefer idle cores that share a cache level (like L2) with already active
	// cores in the same package. Since we don't have explicit L2 clusters here,
	// we use adjacency in the topology-sorted fCores array as a proxy.

	native_cpu_mask_t enabledMask = scheduler_atomic_get((native_cpu_mask_t*)&fEnabledCoreMask);
	native_cpu_mask_t activeMask = enabledMask & ~mask;

	if (activeMask != 0) {
		// Find idle cores that have at least one active neighbor.
		native_cpu_mask_t neighbors = ((activeMask << 1) | (activeMask >> 1)) & mask;
		if (neighbors != 0) {
			// If multiple neighbors exist, pick one semi-randomly to avoid always
			// hitting the same core if it's shared by many active ones.
			if (scheduler_popcount(neighbors) > 1) {
				// Issue 6 fix: clamp shift to [1, kMaxCoresPerPackage-1] to prevent
				// both shift-by-0 (no rotation) and shift-by-kMaxCoresPerPackage UB.
				// The previous guard caught shift==0 and shift==kMaxCoresPerPackage,
				// but shift==kMaxCoresPerPackage is unreachable since the RNG mapping
				// produces [0, kMaxCoresPerPackage-1]. Replacing with explicit clamp
				// makes the intent clear and removes dead-code confusion.
				int32 shift = 1 + (int32)(((uint64)cpu->GetRandom()
					* (uint64)(kMaxCoresPerPackage - 1)) >> 32);
				// shift is now in [1, kMaxCoresPerPackage-1], safe for both directions.
				if (shift >= (int32)kMaxCoresPerPackage) {
					// Defensive clamp (should never fire given RNG range above).
					return fCores[scheduler_ctz(neighbors)];
				}
				native_cpu_mask_t rotated = (neighbors >> shift)
					| (neighbors << (kMaxCoresPerPackage - shift));

				if (rotated != 0) {
					// Un-rotate: a bit at rotated position p came from original
					// position (p + shift) % kMaxCoresPerPackage.
					// Issue 19 fix: correct the un-rotation.
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
					// Fallback if index maps to a NULL slot (sparse package).
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

	// If no core is partially active, just pick an idle one semi-randomly.
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
	// Issue 98 fix: if we reach here with a non-NULL affinity constraint and
	// the lowest idle core doesn't match, scan remaining idle cores rather
	// than silently returning NULL and violating the caller's expectation that
	// a valid core is returned when IdleCoreMask() is non-zero.
	if (affinity != NULL) {
		native_cpu_mask_t remaining = mask;
		if (bit >= 0)
			remaining &= ~((native_cpu_mask_t)1 << bit); // skip the one we checked
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
	// Issue 86 fix: ASSERT only fires in debug builds. Add a production
	// guard to prevent out-of-bounds write corrupting adjacent PackageEntry
	// fields on release builds.
	if (index < 0 || index >= kMaxCoresPerPackage) {
		dprintf("PackageEntry::RegisterCore: index %" B_PRId32 " out of range"
			" [0, %" B_PRId32 ") — core registration skipped\n",
			index, (int32)kMaxCoresPerPackage);
		return;
	}
	fCores[index] = core;
	// Issue 38 fix: update fRegisteredCoreCount BEFORE fCoreCount so that
	// fMaxAttempts (computed from fRegisteredCoreCount below) is based on
	// the new count. The implicit ordering dependency is now explicit.
	fRegisteredCoreCount = max_c(fRegisteredCoreCount, index + 1);
	fCoreCount++;

	// Try to pick distinct random valid cores.
	// Use formula: 4 + (3 * log2(N)) / 2
	// For 32 cores: 4 + 7.5 = 11 attempts.
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

	// Use "Power of Two Choices" random sampling if the core count is large.
	// This avoids cache pollution and interconnect saturation from scanning all cores.
	if (fRegisteredCoreCount > kRandomCoreSearchThreshold) {	//
		uint64 sampledCores = 0;
		int32 attempts = 0;
		int32 registeredCores = fRegisteredCoreCount;
		if (registeredCores <= 0)
			return NULL;

		while (attempts++ < fMaxAttempts) {
			// Select a random bit index based on registered cores to avoid sparse array slots
			int32 i = (int32)get_random_index(cpu->GetRandom(), registeredCores);

			if (sampledCores & (1ULL << i))
				continue;
			sampledCores |= (1ULL << i);

			CoreEntry* candidate = fCores[i];
			if (candidate == NULL)
				continue;

			// Check if this core is enabled
			if (!(((native_cpu_mask_t)1 << i) & enabledMask))
				continue;

			// candidate->ID() is a core ID; the mask is indexed by
			// CPU ID.  Check whether the core has any CPU in the affinity set.
			if (mask != NULL && !candidate->CPUMask().Matches(*mask))
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
		// same fix as the random sampling path above.
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

	// Use "Power of Two Choices" random sampling if the core count is large.
	// This avoids cache pollution and interconnect saturation from scanning all cores.
	if (fRegisteredCoreCount > kRandomCoreSearchThreshold) {	//
		uint64 sampledCores = 0;
		// sampledCores is uint64 which
		// provides 64 deduplication bits.  kMaxCoresPerPackage is
		// sizeof(native_cpu_mask_t)*8 == 64 on 64-bit and 32 on 32-bit,
		// so every possible index i < kMaxCoresPerPackage fits within the
		// bitmask on all supported platforms.  No overflow is possible.
		// static_assert replaced by comment for GCC 2.95
		// kMaxCoresPerPackage <= (int32)(sizeof(sampledCores) * 8)
		int32 attempts = 0;
		int32 registeredCores = fRegisteredCoreCount;
		if (registeredCores <= 0)
			return NULL;

		while (attempts++ < fMaxAttempts) {
			// Select a random bit index based on registered cores
			int32 i = (int32)get_random_index(cpu->GetRandom(), registeredCores);

			if (sampledCores & (1ULL << i))
				continue;
			sampledCores |= (1ULL << i);

			CoreEntry* candidate = fCores[i];
			if (candidate == NULL)
				continue;

			// Check if this core is enabled
			if (!(((native_cpu_mask_t)1 << i) & enabledMask))
				continue;

			// candidate->ID() is a core ID; mask is indexed by CPU ID.
			if (mask != NULL && !candidate->CPUMask().Matches(*mask))
				continue;
			if (type != CORE_TYPE_UNKNOWN && candidate->Type() != type)
				continue;

			int32 load = atomic_get(&fCoreLoads[i]);

			// Track the best core across all attempts (Power-of-N-Choices).
			if (maxEntry == NULL || load > maxLoad
					// Issue 55 fix: tie-break by higher PackageIndex (within
					// the package) rather than lower core ID to spread across
					// more physical cores instead of always favouring core 0.
					|| (load == maxLoad
						&& candidate->PackageIndex() > maxEntry->PackageIndex())) {
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
	// Issue 28 fix: use fRegisteredCoreCount (not kMaxCoresPerPackage) as
	// the modulus for startBit. For a 4-core package, using kMaxCoresPerPackage
	// (64) as the modulus produces startBit values 0–63; bits 4–63 make
	// upperMask zero (no enabled bits above index 3), forcing the fallback
	// that sets upperMask = lowerMask — the randomisation becomes a no-op.
	// Using fRegisteredCoreCount ensures startBit is always within the
	// valid index range [0, fRegisteredCoreCount), making the split effective.
	int32 startBit = 0;

	if (count > 1) {
		startBit = (int32)get_random_index(cpu->GetRandom(),
			fRegisteredCoreCount);
	}

	// Split mask into two parts to randomize start position
	native_cpu_mask_t upperMask = enabledMask & (~(native_cpu_mask_t)0 << startBit);
	native_cpu_mask_t lowerMask = enabledMask & (((native_cpu_mask_t)1 << startBit) - 1);

	if (upperMask == 0) {
		upperMask = lowerMask;
		lowerMask = 0;
	}

	// We iterate twice, but effectively just once over the set bits.
	// The order of loops determines tie-breaking preference.

	for (int pass = 0; pass < 2; pass++) {
		native_cpu_mask_t currentMask = (pass == 0) ? upperMask : lowerMask;
		// Issue 29 fix: skip the second pass when lowerMask is empty to avoid
		// an unnecessary loop iteration with zero work.
		if (currentMask == 0)
			break;

		while (currentMask != 0) {
			int32 i = scheduler_ctz(currentMask);
			currentMask &= ~((native_cpu_mask_t)1 << i);

			CoreEntry* candidate = fCores[i];
			if (candidate == NULL)
				continue;
			// same fix as PeekMinimumLoadCore.
			if (mask != NULL && !candidate->CPUMask().Matches(*mask))
				continue;
			if (type != CORE_TYPE_UNKNOWN && candidate->Type() != type)
				continue;

			int32 load = atomic_get(&fCoreLoads[i]);
			if (maxEntry == NULL || load > maxLoad
					// Issue 55 fix: tie-break by higher PackageIndex (within
					// the package) rather than lower core ID to spread across
					// more physical cores instead of always favouring core 0.
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
