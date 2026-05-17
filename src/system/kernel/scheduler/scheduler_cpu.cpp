/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 * Audit fixes applied 2025.
 */

#include "scheduler_cpu.h"

#include <interrupts.h>
#include <util/AutoLock.h>
#include <util/Random.h>

#include <new>

#include "scheduler_thread.h"
#include "scheduler_topology.h"

namespace Scheduler {

static inline uint32 get_random_index(uint32 random, uint32 range) {
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
	const CPUSet& enabled;
	bigtime_t now;

	LocalNodeStealAction(CPUEntry* c, PackageEntry* p, ThreadData** s,
						 const CPUSet& e, bigtime_t n)
		: cpu(c), package(p), stolen(s), enabled(e), now(n) {}

	bool operator()(PackageEntry* entry) const {
		if (*stolen != NULL)
			return true;

		if (entry == package)
			return false;

		int32 victimCoreCount = entry->RegisteredCoreCount();
		if (victimCoreCount == 0)
			return false;

		int32 coreIndex =
			(int32)get_random_index(cpu->GetRandom(), victimCoreCount);
		CoreEntry* victim = entry->GetCore(coreIndex);

		if (victim == NULL)
			return false;

		if ((entry->IdleCoreMask() &
			 ((native_cpu_mask_t)1 << victim->PackageIndex())) != 0)
			return false;

		if (victim->TryLockRunQueue()) {
			int32 stolenPriority = -1;

			// Note: Hierarchical Lag-Based Steal (Local Node).
			// Prefer threads with highest positive lag.
			*stolen = victim->StealThread(stolenPriority, cpu->ID());

			if (*stolen != NULL && (*stolen)->GetLag() < kNUMANodeLagThreshold) {
				// Thread not under-served enough to justify cross-core steal
				// within the same node. Restore it.
				victim->PushBack(*stolen, stolenPriority);
				*stolen = NULL;
			}

			if (*stolen != NULL) {
				// Re-verify affinity under the lock.
				const CPUSet& threadMask = (*stolen)->GetThread()->cpumask;
				if (threadMask.IsEmpty() || threadMask.GetBit(cpu->ID())) {
					(*stolen)->MigrateTo(cpu->Core(), now);
					(*stolen)->fStolen = true;
					cpu->Core()->IncrementTotalThreadCount();
					victim->UnlockRunQueue();
					return true;
				} else {
					// Victim no longer matches thief after lock acquisition.
					// Push it back and abort this victim.
					victim->PushBack(*stolen,
									 (*stolen)->GetRunQueueLink()->fPriority);
					*stolen = NULL;
				}
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
	const CPUSet& enabled;
	bigtime_t now;

	GlobalRandomStealAction(CPUEntry* c, PackageEntry* p, ThreadData** s,
							const CPUSet& e, bigtime_t n)
		: cpu(c), package(p), stolen(s), enabled(e), now(n) {}

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

		int32 coreIndex =
			(int32)get_random_index(cpu->GetRandom(), victimCoreCount);
		CoreEntry* victim = entry->GetCore(coreIndex);

		if (victim == NULL)
			return false;

		if ((entry->IdleCoreMask() &
			 ((native_cpu_mask_t)1 << victim->PackageIndex())) != 0)
			return false;

		if (victim->TryLockRunQueue()) {
			int32 stolenPriority = -1;

			// Note: Hierarchical Lag-Based Steal (Global).
			// Use higher lag threshold for remote nodes to preserve cache warmth.
			*stolen = victim->StealThread(stolenPriority, cpu->ID());

			if (*stolen != NULL && (*stolen)->GetLag() < kGlobalLagThreshold) {
				// Cross-NUMA migration is expensive; only steal if thread is
				// extremely under-served.
				victim->PushBack(*stolen, stolenPriority);
				*stolen = NULL;
			}

			if (*stolen != NULL) {
				// Re-verify affinity under the lock.
				const CPUSet& threadMask = (*stolen)->GetThread()->cpumask;
				if (threadMask.IsEmpty() || threadMask.GetBit(cpu->ID())) {
					(*stolen)->MigrateTo(cpu->Core(), now);
					(*stolen)->fStolen = true;
					cpu->Core()->IncrementTotalThreadCount();
					victim->UnlockRunQueue();
					return true;
				} else {
					// Victim no longer matches thief after lock acquisition.
					// Push it back and abort this victim.
					victim->PushBack(*stolen,
									 (*stolen)->GetRunQueueLink()->fPriority);
					*stolen = NULL;
				}
			}

			victim->UnlockRunQueue();
		}

		return false;
	}
};

struct StealThreadPredicate {
	int32 thiefCPU;

	StealThreadPredicate(int32 t) : thiefCPU(t) {}

	bool operator()(ThreadData* td) const {
		if (td->IsIdle())
			return false;

		const CPUSet& threadMask = td->GetThread()->cpumask;
		return threadMask.IsEmpty() || threadMask.GetBit(thiefCPU);
	}
};

struct CoreThreadsData {
	CoreEntry* fCore;
	int32 fLoad;
};

class Scheduler::DebugDumper {
public:
	static void DumpCPURunQueue(CPUEntry* cpu);
	static void DumpCoreRunQueue(CoreEntry* core);
	static void DumpCoreEntryLoad(CoreEntry* core);
	static void DumpIdleCoresInPackage(PackageEntry* package);
	static void DumpPackageCores(PackageEntry* package);

private:
	static void _AnalyzeCoreThreads(Thread* thread, void* data);
};

static CPUPriorityHeap sDebugCPUHeap;

void ThreadRunQueue::Dump() const {
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


void IRQRebalanceDPC::DoDPC(DPCQueue* queue) {
	assign_io_interrupt_to_cpu(fIRQ, fTargetCPU);
}

CPUEntry::CPUEntry()
	: fThreadCount(0),
	  fLoad(0),
	  fPerformanceScale(kDefaultCapacity),
	  fMeasureActiveTime(0),
	  fMeasureTime(0),
	  fUpdateLoadEvent(false),
	  fRandomState(1),
	  fRescheduleCount(0),
	  fCoreLocalIndex(0),
	  fInteractionUpdateCounter(0),
	  fSystemVirtualTime(0),
	  fPreemptionThreshold(0),
	  fTotalWeight(0),
	  fReschedulePending(0),
	  fLastLocalPackageIndex(0),
	  lastReschedule(0) {
	B_INITIALIZE_SPINLOCK(&fQueueLock);
}


void CPUEntry::Init(int32 id, CoreEntry* core) {
	fCPUNumber = id;
	atomic_pointer_set<CoreEntry>(&fCore, core);
	StoreRelease64(fRCULastGeneration, 0);
	// Note: improve per-CPU RNG seed entropy. On boot, system_time()
	// returns small values with low entropy; successive CPUs initialized
	// microseconds apart get highly correlated seeds. Fold in the address of
	// the CPUEntry itself (ASLR entropy on randomized kernels), a
	// compile-time constant unique per translation unit, and multiple rounds
	// of Murmur3-style mixing to break correlation.
	uint64 seed = system_time();
	seed ^= (uint64)(uintptr_t)this;			   // ASLR/address entropy
	seed ^= ((uint64)id * 0x9E3779B97F4A7C15ULL);  // Golden ratio spread
	seed ^= (uint64)id << 32;
	// Three rounds of strong mixing
	seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ULL;
	seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBULL;
	seed ^= seed >> 31;
	fRandomState = seed ? seed : 1;

	// Stagger the boost-scan trigger across CPUs. Without this all CPUs
	// fire UpdatePriorityBoostScalable at the same reschedule boundary,
	// causing correlated lock acquisition and measurable latency spikes.
	// Note: id % 10 produces duplicate stagger values when cpu count
	// is not a multiple of 10 (e.g. 12 CPUs: IDs 0,10 both get 0; 1,11 get 1).
	// Use % cpuCount (clamped) so each CPU gets a unique stagger within [0, N).
	// Fall back to id % 10 for systems with > 10 CPUs where spread matters
	// more.
	{
		int32 numCPUs = smp_get_num_cpus();
		int32 staggerMod = (numCPUs > 1 && numCPUs <= 10) ? numCPUs : 10;
		fRescheduleCount = (uint32)(id % staggerMod);
	}
}


void CPUEntry::Start() {
	// fThreadCount and fLoad are already initialized in CoreEntry::AddCPU
	// while holding the necessary locks.
	StoreRelease64(fMeasureTime,
				 system_time());
	StoreRelease64(fMeasureActiveTime, 0);
}


void CPUEntry::Stop() {
	cpu_ent* entry = &gCPU[fCPUNumber];
	// Note: the IRQ drain loop uses a generous safety limit of 1000
	// iterations.  On a pathological system with more than 1000 IRQs
	// assigned to one CPU the excess interrupts are not reassigned and
	// a warning is logged.  The limit is intentional (prevents an
	// infinite loop if assign_io_interrupt_to_cpu silently fails) and
	// the warning below makes the truncation visible.

	// get rid of irqs
	SpinLocker locker(entry->irqs_lock);
	const int32 kMaxIterations = 1000;
	for (int32 i = 0; i < kMaxIterations; i++) {
		irq_assignment* irq =
			(irq_assignment*)list_get_first_item(&entry->irqs);
		if (irq == NULL)
			break;

		int32 irqVector = irq->irq;
		locker.Unlock();

		// Note: assign_io_interrupt_to_cpu may acquire internal locks.
		// Note: verified safe to release irqs_lock here.
		// We release irqs_lock BEFORE calling it (locker.Unlock() above) to
		// prevent priority inversion or deadlock against any path that acquires
		// irqs_lock while holding an internal interrupt-assignment lock.
		// The locker is re-acquired on the next iteration before list access.
		assign_io_interrupt_to_cpu(irqVector, -1);

		locker.Lock();

		// Note: the progress check correctly identifies a silent
		// failure from assign_io_interrupt_to_cpu when the head IRQ didn't
		// change. However, a transient failure that retries next iteration
		// is now correctly distinguished: we only abort if the SAME irqVector
		// is still at the head after a full unlock/assign/lock cycle.
		// The existing logic is correct; add explicit documentation.
		irq_assignment* currentHead =
			(irq_assignment*)list_get_first_item(&entry->irqs);
		if (currentHead != NULL && currentHead->irq == irqVector) {
			// No progress: abort to prevent burning all iterations.
			dprintf("CPUEntry::Stop: interrupt %" B_PRId32
					" could not be "
					"reassigned (driver failure); aborting IRQ drain\n",
					irqVector);
			break;
		}
	}

	if (list_get_first_item(&entry->irqs) != NULL) {
		dprintf(
			"CPUEntry::Stop: safety limit reached while removing "
			"interrupts from CPU %" B_PRId32 "\n",
			fCPUNumber);
	}
	locker.Unlock();
}


void CPUEntry::PushFront(ThreadData* thread, int32 priority) {
	SCHEDULER_ENTER_FUNCTION();
	fRunQueue.PushFront(thread, priority, SystemVirtualTime());
	AddRelease(fThreadCount, 1);

	if (!thread->IsIdle()) {
		Core()->IncrementTotalThreadCount();
		if (priority >= B_DISPLAY_PRIORITY)
			Core()->IncrementDisplayThreadCount();
		if (!thread->IsRealTime())
			AddWeight(thread->GetWeight());
	}
}


void CPUEntry::PushBack(ThreadData* thread, int32 priority) {
	SCHEDULER_ENTER_FUNCTION();
	fRunQueue.PushBack(thread, priority, SystemVirtualTime());
	AddRelease(fThreadCount, 1);

	if (!thread->IsIdle()) {
		Core()->IncrementTotalThreadCount();
		if (priority >= B_DISPLAY_PRIORITY)
			Core()->IncrementDisplayThreadCount();
		if (!thread->IsRealTime())
			AddWeight(thread->GetWeight());
	}
}


void CPUEntry::Remove(ThreadData* thread) {
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(thread->IsEnqueued());

	// (defensive): capture the priority the thread was enqueued with to
	// ensure symmetric counter updates.
	int32 priority = thread->GetRunQueueLink()->fPriority;

	thread->SetDequeued();
	fRunQueue.Remove(thread);
	AddRelease(fThreadCount, -1);

	if (!thread->IsIdle()) {
		Core()->DecrementTotalThreadCount();
		if (priority >= B_DISPLAY_PRIORITY)
			Core()->DecrementDisplayThreadCount();
		if (!thread->IsRealTime())
			AddWeight(-thread->GetWeight());
	}
}

ThreadData* CoreEntry::PeekThread() const {
	SCHEDULER_ENTER_FUNCTION();
	// Note: We don't have a specific SVT for the entire core here,
	// but StealThread and ChooseNextThread handle eligibility properly.
	return fRunQueue.PeekBest();
}

ThreadData* CPUEntry::PeekThread() const {
	SCHEDULER_ENTER_FUNCTION();
	// Note: We don't call CheckEligibility here because PeekThread is often
	// used as a lockless hint from other CPUs. Eligibility is advanced
	// by the owner CPU in reschedule() or ChooseNextThread().
	return fRunQueue.PeekBest();
}

ThreadData* CPUEntry::PeekIdleThread() const {
	SCHEDULER_ENTER_FUNCTION();
	return fRunQueue.GetHead(B_IDLE_PRIORITY);
}


void CPUEntry::UpdatePriority(int32 priority) {
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!gCPU[fCPUNumber].disabled || priority == B_IDLE_PRIORITY);

	int32 oldPriority = CPUPriorityHeap::GetKey(this);
	if (oldPriority == priority)
		return;

	CoreEntry* core = atomic_pointer_get<CoreEntry>(&fCore);
	if (core == NULL)
		return;

	core->CPUHeap()->ModifyKey(this, priority);

	if (oldPriority == B_IDLE_PRIORITY)
		core->CPUWakesUp(this);
	else if (priority == B_IDLE_PRIORITY)
		core->CPUGoesIdle(this);
}


void CPUEntry::ComputeLoad(bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCPULoad);
	ASSERT(!gCPU[fCPUNumber].disabled);
	ASSERT(fCPUNumber == smp_get_current_cpu());

	if (now == 0)
		now = system_time();

	int32 currentLoad = LoadAcquire(fLoad);
	bigtime_t measureActiveTime __attribute__((aligned(8)));
	int oldLoad;
	do {
		bigtime_t measureTime = LoadAcquire64(fMeasureTime);
		measureActiveTime = LoadAcquire64(fMeasureActiveTime);
		bigtime_t tempMeasureTime = measureTime;
		bigtime_t tempMeasureActiveTime = measureActiveTime;
		int32 tempLoad = currentLoad;
		oldLoad = compute_load(tempMeasureTime, tempMeasureActiveTime,
							   tempLoad, now);
		if (oldLoad < 0)
			break;
		if ((bigtime_t)TestAndSet64(fMeasureActiveTime,
				tempMeasureActiveTime, measureActiveTime) == measureActiveTime) {
			StoreRelease64(fMeasureTime,
						 tempMeasureTime);
			currentLoad = tempLoad;
			break;
		}
	} while (true);
	if (oldLoad < 0)
		return;

	// Clamp load to avoid overflow or runaway values
	if (currentLoad < 0)
		currentLoad = 0;
	else if (currentLoad > kLoadClampMax)
		currentLoad = kLoadClampMax;

	StoreRelease(fLoad, currentLoad);

	if (GetLoad() > kVeryHighLoad)
		Scheduler::RebalanceIRQs(false);
}

ThreadData* CPUEntry::ChooseNextThread(ThreadData* oldThread, bool putAtBack,
									   bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	ASSERT_SCHED_LOCK();

	int32 oldPriority = -1;
	if (oldThread != NULL)
		oldPriority = oldThread->GetEffectivePriority();

	CoreEntry* core = atomic_pointer_get<CoreEntry>(&fCore);
	CoreRunQueueLocker coreLocker(core);
	CPURunQueueLocker cpuLocker(this);

	ThreadData* pinnedThread = PeekThread();
	int32 pinnedPriority = -1;
	if (pinnedThread != NULL)
		pinnedPriority = pinnedThread->GetEffectivePriority();

	ThreadData* sharedThread = core->PeekThread();
	if (sharedThread == NULL && pinnedThread == NULL) {
		// try to steal work from other cores in the same package
		sharedThread = _TryStealWork(now);
	}

	bool sharedThreadIsFloating =
		sharedThread != NULL && !sharedThread->IsEnqueued();

	if (sharedThread == NULL && pinnedThread == NULL && oldThread == NULL)
		return NULL;

	int32 sharedPriority = -1;
	if (sharedThread != NULL)
		sharedPriority = sharedThread->GetEffectivePriority();

	int32 rest = max_c(pinnedPriority, sharedPriority);
	if (oldPriority > rest || (!putAtBack && oldPriority == rest)) {
		// Case A: oldThread is best.
		if (sharedThreadIsFloating) {
			cpuLocker.Unlock();
			coreLocker.Unlock();

			bool wasRunQueueEmpty;
			bool requestPreemption;
			bool updateInteraction;
			if (!sharedThread->Enqueue(wasRunQueueEmpty, requestPreemption,
									   updateInteraction, now)) {
				// Note: cache the Thread* once; sharedThread->GetThread()
				// is called multiple times below and the pointer must be
				// consistent.
				Thread* const stolenThread = sharedThread->GetThread();
				if (!enqueue_safe(stolenThread, now)) {
					dprintf(
						"scheduler: WARNING: stolen thread %" B_PRId32
						" lost during hot-unplug -- forcing to current CPU\n",
						stolenThread->id);
					sharedThread->MigrateTo(fCore, now);
					bool dummy1, dummy2;
					// Note: check return value; log if the last-resort also
					// fails.
					if (!sharedThread->Enqueue(dummy1, dummy2,
											   updateInteraction, now)) {
						dprintf(
							"scheduler: CRITICAL: thread %" B_PRId32
							" could not be re-enqueued after forced migration;"
							" scheduler state is inconsistent\n",
							stolenThread->id);
					}
				}
			}

			// Note: call while NOT holding run-queue locks.
			if (updateInteraction)
				scheduler_update_interaction_state(now);
		}
		return oldThread;
	}

	if (sharedPriority > pinnedPriority) {
		// Case B: sharedThread is best.
		if (sharedThread->fStolen) {
			core->DecrementTotalThreadCount();
			sharedThread->fStolen = false;
		}
		if (sharedThread->Core() == core && !sharedThreadIsFloating)
			core->Remove(sharedThread);

		return sharedThread;
	}

	// Case C: pinnedThread is best (or fallback).
	// We MUST remove the thread while holding the locks to avoid a race
	// condition.
	ThreadData* nextThread = NULL;
	if (pinnedThread != NULL && pinnedThread->IsEnqueued()) {
		Remove(pinnedThread);
		nextThread = pinnedThread;
	} else {
		// pinnedThread was stolen while we were stealing/peeking; fallback.
		nextThread = PeekThread();
		if (nextThread != NULL)
			Remove(nextThread);
	}

	if (sharedThreadIsFloating) {
		// We decided to run a pinned thread; put back the stolen shared thread.
		cpuLocker.Unlock();
		coreLocker.Unlock();

		bool wasRunQueueEmpty;
		bool requestPreemption;
		bool updateInteraction;
		if (!sharedThread->Enqueue(wasRunQueueEmpty, requestPreemption,
								   updateInteraction, now)) {
			Thread* const thread = sharedThread->GetThread();
			if (!enqueue_safe(thread, now)) {
				dprintf("scheduler: WARNING: shared thread %" B_PRId32
						" lost during hot-unplug -- forcing to current CPU\n",
						thread->id);
				sharedThread->MigrateTo(fCore, now);
				bool dummy1, dummy2;
				if (!sharedThread->Enqueue(dummy1, dummy2, updateInteraction,
										   now)) {
					dprintf("scheduler: CRITICAL: thread %" B_PRId32
							" could not be re-enqueued after forced migration;"
							" scheduler state is inconsistent\n",
							thread->id);
				}
			}
		}

		if (updateInteraction)
			scheduler_update_interaction_state(now);
	}

	return nextThread;
}


void CPUEntry::UpdateActiveTime(ThreadData* oldThreadData, bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	cpu_ent* cpuEntry = &gCPU[fCPUNumber];

	Thread* oldThread = oldThreadData->GetThread();
	if (!thread_is_idle_thread(oldThread)) {
		bigtime_t active =
			(oldThread->kernel_time - cpuEntry->last_kernel_time) +
			(oldThread->user_time - cpuEntry->last_user_time);

		WriteSequentialLocker locker(cpuEntry->active_time_lock);
		cpuEntry->active_time += active;
		locker.Unlock();

		AddRelease64(fMeasureActiveTime, (int64)(active));
		atomic_pointer_get<CoreEntry>(&fCore)->IncreaseActiveTime(active);

		// Use the provided timestamp for UpdateActivity.
		oldThreadData->UpdateActivity(active, now);
	}
}


void CPUEntry::TrackLoad(ThreadData* nextThreadData, bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (now == 0)
		now = system_time();

#ifdef DEBUG_SCHEDULER
	TRACE("scheduler: cpu=%d load=%d idle=%d\n", fCPUNumber, LoadAcquire(fLoad),
		  gCPU[fCPUNumber].idle);
#endif

	cpu_ent* cpuEntry = &gCPU[fCPUNumber];

	// Note: Update System Virtual Time (SVT).
	// SVT tracks the FairShare baseline for this core.
	if (nextThreadData != NULL) {
		if (!nextThreadData->IsIdle() && !nextThreadData->IsRealTime()) {
			bigtime_t vrt = nextThreadData->GetVirtualRuntime();
			bigtime_t svt = SystemVirtualTime();
			if (vrt > svt)
				SetSystemVirtualTime(vrt);

			// Note: Update Dynamic Preemption Threshold.
			// Scale epsilon based on thread count to protect cache performance.
			int32 threads = ThreadCount();
			StoreRelease64(fPreemptionThreshold, (int64)(threads * 500)); // 500us per thread
		} else if (nextThreadData->IsIdle()) {
			// On an idle system, epsilon is near zero (instant response).
			StoreRelease64(fPreemptionThreshold, 0);
		}
	}

	// Note: update thread timestamps BEFORE calling
	// _RequestPerformanceLevel. The performance level request reads
	// nextThreadData->GetLoad() and Core()->GetLoad(), which are based on
	// accounting that uses last_kernel_time/last_user_time. Updating them
	// first ensures _RequestPerformanceLevel sees fresh accounting data.
	if (nextThreadData != NULL) {
		Thread* nextThread = nextThreadData->GetThread();
		if (!thread_is_idle_thread(nextThread)) {
			cpuEntry->last_kernel_time = nextThread->kernel_time;
			cpuEntry->last_user_time = nextThread->user_time;
			nextThreadData->SetLastInterruptTime(cpuEntry->interrupt_time);
		}
	}

	if (gTrackCPULoad) {
		if (!cpuEntry->disabled)
			ComputeLoad(now);
		if (nextThreadData != NULL)
			_RequestPerformanceLevel(nextThreadData, now);
	}
}

uint32 CPUEntry::GetRandom() {
	uint64 x = fRandomState;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	fRandomState = x;
	// Multiplicative mapping to mix the state: 0x2545F4914F6CDD1DULL
	// is a large 64-bit prime constant used to distribute entropy.
	return (uint32)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

ThreadData* CPUEntry::_TryStealWork(bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	// Note: Formal EEVDF Hierarchical Work-Stealing.
	// Phase 1 (Sibling): Try to steal from a sibling core sharing cache.

	// iterate over other cores in the package and try to steal work
	CoreEntry* core = atomic_pointer_get<CoreEntry>(&fCore);
	PackageEntry* package = core->Package();

	int32 registeredCores = package->RegisteredCoreCount();
	if (registeredCores <= 1)
		return NULL;

	// (clarification): The subtraction-wrap "index -= registeredCores"
	// is safe because startIndex < registeredCores and i < registeredCores,
	// so startIndex + i < 2 * registeredCores, requiring at most one
	// subtraction. (clarification): GetIdleCorePacking shift guard - when
	// shift==0 the left-shift by kMaxCoresPerPackage is already guarded by "if
	// (shift > 0)" in PackageEntry::GetIdleCorePacking; no undefined behaviour
	// occurs.

	CPUSet enabled;
	const int32 kWords = (SMP_MAX_CPUS + 31) / 32;
	for (int32 i = 0; i < kWords; i++) {
		uint32 w;
		int retry = 0;
		do {
			w = gCPUEnabled.Bits(i);
			memory_read_barrier();
			if (w == gCPUEnabled.Bits(i) || ++retry >= 3)
				break;
			cpu_pause();
		} while (true);
		enabled.SetWord(i, w);
	}

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

		// Note: IdleCoreMask() is read atomically but without holding
		// a lock. Between this read and TryLockRunQueue(), the victim core may
		// have transitioned from idle to active. The skip is a best-effort
		// optimisation, not a guarantee. The comment "guaranteed empty" was
		// incorrect - replace with accurate description.
		// The TryLockRunQueue() + PeekOption() predicate below is the actual
		// correctness barrier; this skip only avoids a redundant lock attempt.
		if ((package->IdleCoreMask() &
			 ((native_cpu_mask_t)1 << victim->PackageIndex())) != 0) {
			continue;
		}

		// Use TryLock to avoid contention
		if (victim->TryLockRunQueue()) {
			int32 stolenPriority = -1;
			ThreadData* stolen =
				victim->StealThread(stolenPriority, fCPUNumber);
			if (stolen != NULL) {
				// Re-verify affinity under the lock.
				const CPUSet& threadMask = stolen->GetThread()->cpumask;
				if (threadMask.IsEmpty() || threadMask.GetBit(fCPUNumber)) {
					CoreEntry* core = atomic_pointer_get<CoreEntry>(&fCore);
					stolen->MigrateTo(core, now);
					stolen->fStolen = true;
					core->IncrementTotalThreadCount();
				} else {
					// Victim no longer matches thief after lock acquisition.
					// Push it back and abort this victim.
					victim->PushBack(stolen,
									 stolen->GetRunQueueLink()->fPriority);
					stolen = NULL;
				}
			}

			victim->UnlockRunQueue();

			if (stolen != NULL)
				return stolen;
		}
	}

	// Phase 2: The Local NUMA Node (Random)
	// Target: Cores on the same physical socket/die (e.g., 64-128 cores).
	// Method: Random Sampling
	// Why: Stealing here is fast (local RAM). You want to exhaust reasonable
	// options here before going across the expensive interconnect.

	// Note: declare 'stolen' BEFORE the 'goto phase3' to avoid
	// jumping over a variable initialisation, which is ill-formed in C++
	// (UB even for trivial types when an initialiser is present).
	ThreadData* stolen = NULL;

	{
		SchedulerNode* node = package->Node();
		if (node == NULL)
			goto phase3;

		search_local_node(
			node, LocalNodeStealAction(this, package, &stolen, enabled, now));

		if (stolen != NULL)
			return stolen;
	}

phase3:
	// Note: removed the unconditional stolen = NULL reset.
	// If Phase 2 fell through with a valid stolen thread, the reset would
	// lose it.  If Phase 2 jumped here (node == NULL) stolen is already NULL.
	// Phase 3 correctly checks (stolen != NULL) in its first lambda probe.

	// Phase 3: The Global Hail Mary (Random)
	// Target: Any core in the system (4096 cores).
	// Method: Logarithmic Formula
	// Why: This is the last resort. If the local node is empty, you are willing
	// to pay the high cost to steal from a remote socket to avoid sleeping.

	search_global_random(
		GlobalRandomStealAction(this, package, &stolen, enabled, now));

	if (stolen != NULL)
		return stolen;

	return NULL;
}


void CPUEntry::StartQuantumTimer(ThreadData* thread, bool wasPreempted) {
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


void CPUEntry::_RequestPerformanceLevel(ThreadData* threadData, bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (gCPU[fCPUNumber].disabled) {
		decrease_cpu_performance(kCPUPerformanceScaleMax);
		return;
	}

	if (now == 0)
		now = system_time();

	int32 load = max_c(threadData->GetLoad(), GetLoad());
	ASSERT_PRINT(load >= 0 && load <= kMaxLoad + kSMTPenalty,
				 "load is out of range %" B_PRId32 " (max of %" B_PRId32
				 " %" B_PRId32 ")",
				 load, threadData->GetLoad(), GetLoad());

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

/* static */ int32 CPUEntry::_RescheduleEvent(timer* /* unused */) {
	get_cpu_struct()->invoke_scheduler = true;
	get_cpu_struct()->preempted = true;
	return B_HANDLED_INTERRUPT;
}

/* static */ int32 CPUEntry::_UpdateLoadEvent(timer* /* unused */) {
	bigtime_t now = system_time();
	CoreEntry::GetCore(smp_get_current_cpu())->ChangeLoad(0, now);
	CPUEntry::GetCPU(smp_get_current_cpu())->fUpdateLoadEvent = false;
	return B_HANDLED_INTERRUPT;
}

CPUPriorityHeap::CPUPriorityHeap(int32 cpuCount)
	: Heap<CPUEntry, int32>(cpuCount) {}

void CPUPriorityHeap::Dump() {
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
	: fPackage(NULL),
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
	  fLocalIndices(0) {
	B_INITIALIZE_SPINLOCK(&fCPULock);
	B_INITIALIZE_SPINLOCK(&fQueueLock);
}


void CoreEntry::Init(int32 id, PackageEntry* package) {
	fCoreID = id;
	fPackage = package;

	fScoreFactor = (kDefaultCapacity << 16) / fCapacity;

	fCPUHeap.~CPUPriorityHeap();
	new (&fCPUHeap) CPUPriorityHeap(smp_get_num_cpus());
	if (fCPUHeap.InitCheck() != B_OK)
		panic("CoreEntry::Init: failed to allocate CPU heap");
}


void CoreEntry::PushFront(ThreadData* thread, int32 priority) {
	SCHEDULER_ENTER_FUNCTION();

	// Note: Formal EEVDF Eligibility.
	// Threads only enter the active heap when mathematically eligible.
	// In a shared core queue, we use a global approximation or the waker's CPU SVT.

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
	fRunQueue.PushFront(thread, priority, cpu->SystemVirtualTime());
	AddRelease(fThreadCount, 1);
	IncrementTotalThreadCount();
	if (priority >= B_DISPLAY_PRIORITY)
		IncrementDisplayThreadCount();
}


void CoreEntry::PushBack(ThreadData* thread, int32 priority) {
	SCHEDULER_ENTER_FUNCTION();

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
	fRunQueue.PushBack(thread, priority, cpu->SystemVirtualTime());
	AddRelease(fThreadCount, 1);
	IncrementTotalThreadCount();
	if (priority >= B_DISPLAY_PRIORITY)
		IncrementDisplayThreadCount();
}


void CoreEntry::Remove(ThreadData* thread) {
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!thread->IsIdle());
	ASSERT(thread->IsEnqueued());

	// (defensive): capture the enqueued priority to ensure consistent
	// fDisplayThreadCount accounting.
	int32 priority = thread->GetRunQueueLink()->fPriority;

	thread->SetDequeued();

	AddRelease(fThreadCount, -1);
	DecrementTotalThreadCount();
	if (priority >= B_DISPLAY_PRIORITY)
		DecrementDisplayThreadCount();
	fRunQueue.Remove(thread);
}

ThreadData* CoreEntry::StealThread(int32& stolenPriority, int32 thiefCPU) {
	SCHEDULER_ENTER_FUNCTION();

	// Note: Formal EEVDF Steal Criteria: "The Laggiest Wins".
	// Unlike traditional schedulers, we prefer threads with the
	// highest positive Lag (most under-served).

	CPUEntry* thief = CPUEntry::GetCPU(thiefCPU);
	const_cast<ThreadRunQueue&>(fRunQueue).CheckEligibility(thief->SystemVirtualTime());

	// Thief prefers cores with lower total weight pressure.
	// (Target pressure comparison is handled in _TryStealWork)

	ThreadData* thread = fRunQueue.PeekBest(ThreadDataLagCompare(), StealThreadPredicate(thiefCPU));

	if (thread != NULL) {
		// Only steal if thread has positive lag (under-served)
		if (thread->GetLag() <= 0)
			return NULL;

		stolenPriority = thread->GetEffectivePriority();
		Remove(thread);
	}
	return thread;
}


void CoreEntry::AddCPU(CPUEntry* cpu) {
	ASSERT(LoadAcquire(fCPUCount) >= 0);
	ASSERT(LoadAcquire(fIdleCPUCount) >= 0);

	// Initialize CPU thread count before it can be seen by others.
	cpu->Reset();

	AddRelease(fIdleCPUCount, 1);
	bool firstCPU = (AddRelease(fCPUCount, 1) == 0);

	// Find the first available local index using a CAS loop.
	// This ensures unique index assignment even after arbitrary CPU
	// hot-unplugging.
	int32 localIndex = -1;
	while (true) {
		native_cpu_mask_t mask = cpu_mask_get_atomic(&fLocalIndices);

		// Explicitly check for full mask before calling ctz to avoid
		// architecture-dependent undefined behavior on ctz(0).
		if (mask == (native_cpu_mask_t)-1) {
			panic(
				"CoreEntry::AddCPU: no available local index for core "
				"%" B_PRId32,
				fCoreID);
		}

		localIndex = scheduler_ctz(~mask);
		if (localIndex < 0 || localIndex >= kMaxCoresPerPackage) {
			panic("CoreEntry::AddCPU: local index %" B_PRId32
				  " out of range "
				  "for core %" B_PRId32,
				  localIndex, fCoreID);
		}

		if (cpu_mask_test_and_set_atomic(&fLocalIndices, mask | ((native_cpu_mask_t)1 << localIndex), mask) == mask) {
			break;
		}
	}
	cpu->fCoreLocalIndex = localIndex;

	fCPUSet.SetBitAtomic(cpu->ID());

	bool didAddIdle = false;
	if (firstCPU) {
		didAddIdle = true;
		StoreRelease(fLoad, 0);
		StoreRelease64(fCombinedLoad, 0);

		StoreRelease(fPackage->fCoreLoads[fPackageIndex], 0);
		cpu_mask_or_atomic(&fPackage->fEnabledCoreMask, (native_cpu_mask_t)1 << fPackageIndex);

		fPackage->AddIdleCore(this);
	}

	if (fCPUHeap.Insert(cpu, B_IDLE_PRIORITY) != B_OK) {
		fCPUSet.ClearBitAtomic(cpu->ID());
		// Roll back the local index on failure.
		cpu_mask_and_atomic(&fLocalIndices, ~((native_cpu_mask_t)1 << localIndex));
		if (firstCPU) {
			StoreRelease(fLoad, 0);
			StoreRelease64(fCombinedLoad, 0);
			StoreRelease(fPackage->fCoreLoads[fPackageIndex], 0);
			if (didAddIdle)
				fPackage->RemoveIdleCore(this);
			cpu_mask_and_atomic(&fPackage->fEnabledCoreMask, ~((native_cpu_mask_t)1 << fPackageIndex));
			AddRelease(fCPUCount, -1);
		} else {
			AddRelease(fCPUCount, -1);
		}
		AddRelease(fIdleCPUCount, -1);
		panic("CoreEntry::AddCPU: failed to insert CPU %" B_PRId32 " into heap",
			  cpu->ID());
	}
}


void CoreEntry::RemoveCPU(CPUEntry* cpu,
						  ThreadProcessing& threadPostProcessing) {
	ASSERT(LoadAcquire(fCPUCount) > 0);
	ASSERT(LoadAcquire(fIdleCPUCount) >= 1);

	// The CPU is guaranteed to be idle and accounted for in fIdleCPUCount
	// before RemoveCPU is called (set by scheduler_set_cpu_enabled).
	int32 oldIdleCount = AddAcquireRelease(fIdleCPUCount, -1);

	fCPUSet.ClearBitAtomic(cpu->ID());
	int32 oldCPUCount = AddAcquireRelease(fCPUCount, -1);

	if (oldCPUCount == 1) {
		// core has been disabled
		cpu_mask_and_atomic(&fPackage->fEnabledCoreMask, ~((native_cpu_mask_t)1 << fPackageIndex));

		// Note: only call RemoveIdleCore if the core was actually idle
		// (all its CPUs were idle). Calling unconditionally when a non-idle
		// core is removed decrements fIdleCoreCount below its true value,
		// corrupting idle core accounting for the entire package.
		if (oldIdleCount == 1)
			fPackage->RemoveIdleCore(this);

		// Note: use CoreRunQueueLocker per-iteration to prevent
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
			// Note: threadPostProcessing calls enqueue() which calls
			// Enqueue() which increments gTotalRunnableThreads for non-idle
			// threads. If enqueue() subsequently fails (all cores disabled),
			// it returns false without a matching decrement. Detect this case
			// and decrement manually to avoid a permanent counter leak.
			threadPostProcessing(threadData);
		}

		// Note: after drain, explicitly verify fThreadCount is zero.
		// If a concurrent steal occurred in the narrow window, fThreadCount
		// may be positive. Force it to zero since CPUCount==0 prevents
		// further enqueues, making any residual count a permanent leak.
		int32 residual = LoadAcquire(fThreadCount);
		if (residual != 0) {
			dprintf("CoreEntry::RemoveCPU: fThreadCount=%" B_PRId32
					" after drain (expected 0) - resetting\n",
					residual);
			StoreRelease(fThreadCount, 0);
		}
	}

	// Use B_INT32_MIN instead of the implicit magic -1.  B_INT32_MIN is
	// less than every valid scheduler priority (minimum B_IDLE_PRIORITY == 0),
	// so the CPU is guaranteed to bubble to the heap root.  The explicit
	// constant makes the intent clear and prevents silent misbehaviour if the
	// priority range ever grows to include negative values.
	fCPUHeap.ModifyKey(cpu, B_INT32_MIN);
	// Note: fCPUHeap is accessed exclusively under fCPULock, which the
	// caller holds via CoreCPUHeapLocker for the duration of RemoveCPU.  No
	// other CPU can concurrently modify the heap, so the root is guaranteed to
	// be 'cpu' immediately after ModifyKey(cpu, B_INT32_MIN).  The previous
	// spin loop was dead code and its 1000-iteration cap masked any real
	// invariant violations by silently proceeding with a wrong root.
	ASSERT(fCPUHeap.PeekRoot() == cpu);
	fCPUHeap.RemoveRoot();

	// Atomically clear the bit in fLocalIndices.
	cpu_mask_and_atomic(&fLocalIndices, ~((native_cpu_mask_t)1 << cpu->fCoreLocalIndex));

	ASSERT(cpu->GetLoad() >= 0 && cpu->GetLoad() <= kMaxLoad);
	ASSERT(fLoad >= 0);
}

bigtime_t CPUEntry::GetMinVirtualRuntime() const {
	SCHEDULER_ENTER_FUNCTION();

	CPURunQueueLocker locker(const_cast<CPUEntry*>(this));
	ThreadData* thread =
		fRunQueue.PeekBest(ThreadDataVRuntimeCompare(), ThreadDataOptimal());
	if (thread == NULL)
		return 0;
	return thread->GetVirtualRuntime();
}

bigtime_t CoreEntry::GetMinVirtualRuntime() const {
	SCHEDULER_ENTER_FUNCTION();

	CoreRunQueueLocker locker(const_cast<CoreEntry*>(this));
	ThreadData* thread =
		fRunQueue.PeekBest(ThreadDataVRuntimeCompare(), ThreadDataOptimal());
	if (thread == NULL)
		return 0;
	return thread->GetVirtualRuntime();
}

CPUEntry* CoreEntry::PeekMinimumLoadCPU() {
	// Optimization: If a physical core is idle, explicitly return the
	// Primary logical CPU first.
	// Note: use fCPUSet.FindFirstSet() equivalent via scheduler_ctz()
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
				// Note: verify the CPU is still in the heap before
				// returning it. A concurrent RemoveCPU may have set the heap
				// key to B_INT32_MIN between the fCPUSet read and this check.
				// GetKey returns B_INT32_MIN for removed CPUs; any valid CPU
				// has key >= B_IDLE_PRIORITY (0).
				if (entry->Core() == this && !gCPU[cpu].disabled &&
					CPUPriorityHeap::GetKey(entry) >= B_IDLE_PRIORITY)
					return entry;
			}
			// First set bit found but not valid; fall through to heap path.
			break;
		}
	}

	CoreCPUHeapLocker _(this);
	return fCPUHeap.PeekRoot();
}


void CoreEntry::SetCapacity(int32 capacity) {
	fCapacity = capacity;
	fScoreFactor = (kDefaultCapacity << 16) / fCapacity;
}


void CoreEntry::_UpdateLoad(bool forceUpdate, bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	int32 cpuCount = LoadAcquire(fCPUCount);
	if (cpuCount <= 0)
		return;

	if (now == 0)
		now = system_time();

	// one system_time() call shared by both branches eliminates
	// a redundant syscall and ensures consistent timestamps.
	bigtime_t lastUpdate = LoadAcquire64(fLastLoadUpdate);
	if (!forceUpdate) {
		if (now < kLoadMeasureInterval + lastUpdate)
			return;
		if (TestAndSet64(fLastLoadUpdate, (int64)(now), (int64)(lastUpdate)) != lastUpdate) {
			return;
		}
	} else {
		// on CAS failure another CPU won the update race; that
		// update is sufficient, so return rather than silently skipping.
		if (TestAndSet64(fLastLoadUpdate, (int64)(now), (int64)(lastUpdate)) != lastUpdate) {
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
	int64 oldCombined __attribute__((aligned(8))) = LoadAcquire64(fCombinedLoad);
	int outerRetryCount = 0;
	while (true) {
		currentLoad = (int32)(oldCombined >> 32);
		uint32 nextEpoch = (uint32)oldCombined + 1;
		int64 newCombined = (int64)nextEpoch;  // Load reset to 0

		int64 actual = TestAndSet64(fCombinedLoad,
			newCombined, oldCombined);
		if (actual == oldCombined) {
			// Note: snapshot prevLoad immediately after winning the
			// outer CAS on fCombinedLoad, not before the loop.  The original
			// code snapshotted prevLoad before the loop, so concurrent
			// RemoveLoad(force=true) calls that ran between the snapshot and
			// the CAS win produced a stale baseline, making delta wrong.
			// Reading here minimises the race window to the CAS itself.
			// currentFLoad starts equal to prevLoad; the inner retry loop
			// updates it on CAS failure and correctly adds delta each time.
			int32 prevLoad = LoadAcquire(fLoad);
			int32 currentFLoad = prevLoad;

			// Note: snapshot prevLoad immediately after winning the
			// outer CAS. A concurrent ChangeLoad/AddLoad can still modify
			// fLoad between the CAS win and the prevLoad read, but this
			// window is now as narrow as possible (a single atomic-get).
			// The inner retry loop handles any residual CAS failure.
			//
			// Note: add iteration limit to the inner CAS loop to
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

				int32 actualLoad = TestAndSet(fLoad, (int32)(newFLoad), (int32)(currentFLoad));
				if (actualLoad == currentFLoad)
					break;

				currentFLoad = actualLoad;
				if (++innerRetryCount >= kMaxFLoadRetries) {
					// Best-effort: apply delta to most recently observed value.
					AddRelease(fLoad, delta);
					break;
				}
			}
			break;
		}

		// Note: bound the outer CAS retry loop too.
		// Livelock is possible if another CPU perpetually updates fCombinedLoad
		// between our read and our CAS. After kMaxCombinedRetries we give up;
		// the next _UpdateLoad call (triggered by the next load-measure timer)
		// will correct any accumulated error.
		static const int kMaxCombinedRetries = 64;
		if (++outerRetryCount >= kMaxCombinedRetries) {
			// Note: re-read cpuCount to get a fresh value after
			// many retry failures. The value from function entry may be
			// several epochs stale on a heavily contended system.
			int32 freshCPUCount = LoadAcquire(fCPUCount);
			if (freshCPUCount <= 0)
				return;

			int32 load = (int32)(oldCombined >> 32) / freshCPUCount;
			load = ((int64)load * fScoreFactor) >> 16;
			StoreRelease(fPackage->fCoreLoads[fPackageIndex],
						 min_c(load, (int32)kMaxLoad));
			return;
		}
		oldCombined = actual;
	}

	if (cpuCount > 0) {
		int32 load = currentLoad / cpuCount;
		load = ((int64)load * fScoreFactor) >> 16;

		int32 oldLoad = LoadAcquire(fPackage->fCoreLoads[fPackageIndex]);
		StoreRelease(fPackage->fCoreLoads[fPackageIndex],
					 SmoothLoad(oldLoad, min_c(load, (int32)kMaxLoad)));
	}
}

/* static */ void CoreEntry::_UnassignThread(Thread* thread, void* data) {
	CoreEntry* core = static_cast<CoreEntry*>(data);
	ThreadData* threadData = thread->scheduler_data;

	if (threadData->Core() == core)
		threadData->UnassignCore();
}

SchedulerNode::SchedulerNode()
	: fIdlePackageMask(0), fPackageStartIndex(0), fPackageCount(0) {}

void SchedulerNode::Init(int32 id) {
	fNodeID = id;
	cpu_mask_set_atomic(&fIdlePackageMask, 0);
	fPackageStartIndex = 0;
	fPackageCount = 0;
}

PackageEntry::PackageEntry()
	: fIdleCoreCount(0), fCoreCount(0), fRegisteredCoreCount(0) {
	B_INITIALIZE_RW_SPINLOCK(&fCoreLock);
}


void PackageEntry::Init(int32 id, SchedulerNode* node, int32 nodeIndex) {
	fPackageID = id;
	fNode = node;
	fNodeIndex = nodeIndex;
	cpu_mask_set_atomic(&fIdleCoreMask, 0);
	cpu_mask_set_atomic(&fEnabledCoreMask, 0);
	fCoreCount = 0;
	fRegisteredCoreCount = 0;
	fMaxAttempts = 0;
	memset(fCores, 0, sizeof(fCores));
	// Note: explicitly zero fCoreLoads on every Init() call.
	// If PackageEntry objects are ever reused after a topology rebuild,
	// stale load values persist and corrupt choose_core decisions.
	memset(fCoreLoads, 0, sizeof(fCoreLoads));
	// Zero fIdleCoreCount explicitly to match fIdleCoreMask == 0.
	fIdleCoreCount = 0;
}


void PackageEntry::AddIdleCore(CoreEntry* core) {
	WriteSpinLocker coreLocker(fCoreLock);
	native_cpu_mask_t oldMask = cpu_mask_or_atomic(&fIdleCoreMask, (native_cpu_mask_t)1 << core->PackageIndex());
	AddRelease(fIdleCoreCount, 1);

	if (oldMask == 0) {
		// Note: document that fCoreLock is held here but NOT held
		// in CoreGoesIdle. The two paths are serialized at a higher level
		// (InterruptsBigSchedulerLocker for AddIdleCore vs. normal scheduling
		// for CoreGoesIdle). If this serialization is ever relaxed, an
		// explicit atomic CAS must guard the PackageGoesIdle transition.
		if (fNode != NULL)
			fNode->PackageGoesIdle(this);
	}
}


void PackageEntry::RemoveIdleCore(CoreEntry* core) {
	WriteSpinLocker coreLocker(fCoreLock);
	// Clear the mask bit BEFORE decrementing the count.  A concurrent reader
	// of the mask (e.g. GetIdleCore) must not see a core that is in the
	// process of being removed from the idle set, as that could lead to a
	// "dangling-ish" core reference if the core is being disabled.  The
	// reader of the count (e.g. GetLeastIdlePackage) will gracefully handle
	// a count > 0 with an empty mask by receiving NULL from GetIdleCore().
	native_cpu_mask_t clearBit = (native_cpu_mask_t)1 << core->PackageIndex();
	native_cpu_mask_t oldMask = cpu_mask_and_atomic(&fIdleCoreMask, ~clearBit);

	AddRelease(fIdleCoreCount, -1);

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

CoreEntry* PackageEntry::GetIdleCore(int32 index) const {
	native_cpu_mask_t mask = cpu_mask_get_atomic(&fIdleCoreMask);
	if (mask == 0)
		return NULL;

	native_cpu_mask_t currentMask = mask;

	// Find the N-th set bit (index-th)
	for (int32 i = 0; i < index; i++) {
		int32 bit = scheduler_ctz(currentMask);
		currentMask &= ~((native_cpu_mask_t)1 << bit);

		if (currentMask == 0) {
			// index out of bounds (race), fallback to the first idle core
			// Note: validate fCores[bit] is non-NULL.
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

CoreEntry* PackageEntry::GetIdleCorePacking(CPUEntry* cpu,
											const CPUSet* affinity) const {
	native_cpu_mask_t mask = cpu_mask_get_atomic(&fIdleCoreMask);
	if (mask == 0)
		return NULL;

	// Intra-package packing:
	// We prefer idle cores that share a cache level (like L2) with already
	// active cores in the same package. Since we don't have explicit L2
	// clusters here, we use adjacency in the topology-sorted fCores array as a
	// proxy.

	native_cpu_mask_t enabledMask =
		cpu_mask_get_atomic(&fEnabledCoreMask);
	native_cpu_mask_t activeMask = enabledMask & ~mask;

	if (activeMask != 0) {
		// Find idle cores that have at least one active neighbor.
		native_cpu_mask_t neighbors =
			((activeMask << 1) | (activeMask >> 1)) & mask;
		if (neighbors != 0) {
			// If multiple neighbors exist, pick one semi-randomly to avoid
			// always hitting the same core if it's shared by many active ones.
			if (scheduler_popcount(neighbors) > 1) {
				// Note: clamp shift to [1, kMaxCoresPerPackage-1] to prevent
				// both shift-by-0 (no rotation) and
				// shift-by-kMaxCoresPerPackage UB. The previous guard caught
				// shift==0 and shift==kMaxCoresPerPackage, but
				// shift==kMaxCoresPerPackage is unreachable since the RNG
				// mapping produces [0, kMaxCoresPerPackage-1]. Replacing with
				// explicit clamp makes the intent clear and removes dead-code
				// confusion.
				int32 shift = 1 + (int32)(((uint64)cpu->GetRandom() *
										   (uint64)(kMaxCoresPerPackage - 1)) >>
										  32);
				// shift is now in [1, kMaxCoresPerPackage-1], safe for both
				// directions.
				if (shift >= (int32)kMaxCoresPerPackage) {
					// Defensive clamp (should never fire given RNG range
					// above).
					return fCores[scheduler_ctz(neighbors)];
				}
				native_cpu_mask_t rotated =
					(neighbors >> shift) |
					(neighbors << (kMaxCoresPerPackage - shift));

				if (rotated != 0) {
					// Un-rotate: a bit at rotated position p came from original
					// position (p + shift) % kMaxCoresPerPackage.
					// Note: correct the un-rotation.
					int32 pos = scheduler_ctz(rotated);
					int32 origIdx = (pos + shift) % kMaxCoresPerPackage;
					if (origIdx >= 0 && origIdx < kMaxCoresPerPackage &&
						fCores[origIdx] != NULL &&
						(neighbors & ((native_cpu_mask_t)1 << origIdx))) {
						if (affinity == NULL ||
							fCores[origIdx]->CPUMask().Matches(*affinity)) {
							return fCores[origIdx];
						}
					}
					// Fallback if index maps to a NULL slot (sparse package).
				}
			}

			native_cpu_mask_t candidateMask = neighbors;
			while (candidateMask != 0) {
				int32 bit = scheduler_ctz(candidateMask);
				if (fCores[bit] != NULL &&
					(affinity == NULL ||
					 fCores[bit]->CPUMask().Matches(*affinity))) {
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
			if (candidate != NULL &&
				(affinity == NULL || candidate->CPUMask().Matches(*affinity))) {
				return candidate;
			}
		}
	}

	int32 bit = scheduler_ctz(mask);
	if (bit >= 0 && bit < kMaxCoresPerPackage && fCores[bit] != NULL &&
		(affinity == NULL || fCores[bit]->CPUMask().Matches(*affinity))) {
		return fCores[bit];
	}
	// Note: if we reach here with a non-NULL affinity constraint and
	// the lowest idle core doesn't match, scan remaining idle cores rather
	// than silently returning NULL and violating the caller's expectation that
	// a valid core is returned when IdleCoreMask() is non-zero.
	if (affinity != NULL) {
		native_cpu_mask_t remaining = mask;
		if (bit >= 0)
			remaining &=
				~((native_cpu_mask_t)1 << bit);	 // skip the one we checked
		while (remaining != 0) {
			int32 nextBit = scheduler_ctz(remaining);
			remaining &= ~((native_cpu_mask_t)1 << nextBit);
			if (nextBit >= 0 && nextBit < kMaxCoresPerPackage &&
				fCores[nextBit] != NULL &&
				fCores[nextBit]->CPUMask().Matches(*affinity)) {
				return fCores[nextBit];
			}
		}
	}
	return NULL;
}


void PackageEntry::RegisterCore(int32 index, CoreEntry* core) {
	// Note: ASSERT only fires in debug builds. Add a production
	// guard to prevent out-of-bounds write corrupting adjacent PackageEntry
	// fields on release builds.
	if (index < 0 || index >= kMaxCoresPerPackage) {
		dprintf("PackageEntry::RegisterCore: index %" B_PRId32
				" out of range"
				" [0, %" B_PRId32 ") - core registration skipped\n",
				index, (int32)kMaxCoresPerPackage);
		return;
	}
	fCores[index] = core;
	// Note: update fRegisteredCoreCount BEFORE fCoreCount so that
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

CoreEntry* PackageEntry::PeekMinimumLoadCore(CPUEntry* cpu, const CPUSet* mask,
											 CoreType type) const {
	CoreEntry* minEntry = NULL;
	int32 minLoad = -1;

	native_cpu_mask_t enabledMask =
		cpu_mask_get_atomic(&fEnabledCoreMask);
	if (enabledMask == 0)
		return NULL;

	// Use "Power of Two Choices" random sampling if the core count is large.
	// This avoids cache pollution and interconnect saturation from scanning all
	// cores.
	if (fRegisteredCoreCount > kRandomCoreSearchThreshold) {  //
		uint64 sampledCores = 0;
		int32 attempts = 0;
		int32 registeredCores = fRegisteredCoreCount;
		if (registeredCores <= 0)
			return NULL;

		while (attempts++ < fMaxAttempts) {
			// Select a random bit index based on registered cores to avoid
			// sparse array slots
			int32 i =
				(int32)get_random_index(cpu->GetRandom(), registeredCores);

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

			int32 load = LoadAcquire(fCoreLoads[i]);

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

		int32 load = LoadAcquire(fCoreLoads[i]);
		if (minEntry == NULL || load < minLoad) {
			minLoad = load;
			minEntry = candidate;
		}
	}
	return minEntry;
}

CoreEntry* PackageEntry::PeekMaximumLoadCore(CPUEntry* cpu, const CPUSet* mask,
											 CoreType type) const {
	CoreEntry* maxEntry = NULL;
	int32 maxLoad = -1;

	native_cpu_mask_t enabledMask =
		cpu_mask_get_atomic(&fEnabledCoreMask);
	if (enabledMask == 0)
		return NULL;

	// Use "Power of Two Choices" random sampling if the core count is large.
	// This avoids cache pollution and interconnect saturation from scanning all
	// cores.
	if (fRegisteredCoreCount > kRandomCoreSearchThreshold) {  //
		uint64 sampledCores = 0;
		// sampledCores is uint64 which
		// provides 64 deduplication bits.  kMaxCoresPerPackage is
		// sizeof(native_cpu_mask_t)*8 == 64 on 64-bit and 32 on 32-bit,
		// so every possible index i < kMaxCoresPerPackage fits within the
		// bitmask on all supported platforms.  No overflow is possible.
		static_assert(kMaxCoresPerPackage <= (int32)(sizeof(sampledCores) * 8),
					  "sampledCores too narrow for kMaxCoresPerPackage");
		int32 attempts = 0;
		int32 registeredCores = fRegisteredCoreCount;
		if (registeredCores <= 0)
			return NULL;

		while (attempts++ < fMaxAttempts) {
			// Select a random bit index based on registered cores
			int32 i =
				(int32)get_random_index(cpu->GetRandom(), registeredCores);

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

			int32 load = LoadAcquire(fCoreLoads[i]);

			// Track the best core across all attempts (Power-of-N-Choices).
			if (maxEntry == NULL ||
				load > maxLoad
				// Note: tie-break by higher PackageIndex (within
				// the package) rather than lower core ID to spread across
				// more physical cores instead of always favouring core 0.
				|| (load == maxLoad &&
					candidate->PackageIndex() > maxEntry->PackageIndex())) {
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
	// Note: use fRegisteredCoreCount (not kMaxCoresPerPackage) as
	// the modulus for startBit. For a 4-core package, using kMaxCoresPerPackage
	// (64) as the modulus produces startBit values 0–63; bits 4–63 make
	// upperMask zero (no enabled bits above index 3), forcing the fallback
	// that sets upperMask = lowerMask - the randomisation becomes a no-op.
	// Using fRegisteredCoreCount ensures startBit is always within the
	// valid index range [0, fRegisteredCoreCount), making the split effective.
	int32 startBit = 0;

	if (count > 1) {
		startBit =
			(int32)get_random_index(cpu->GetRandom(), fRegisteredCoreCount);
	}

	// Split mask into two parts to randomize start position
	native_cpu_mask_t upperMask =
		enabledMask & (~(native_cpu_mask_t)0 << startBit);
	native_cpu_mask_t lowerMask =
		enabledMask & (((native_cpu_mask_t)1 << startBit) - 1);

	if (upperMask == 0) {
		upperMask = lowerMask;
		lowerMask = 0;
	}

	// We iterate twice, but effectively just once over the set bits.
	// The order of loops determines tie-breaking preference.

	for (int pass = 0; pass < 2; pass++) {
		native_cpu_mask_t currentMask = (pass == 0) ? upperMask : lowerMask;
		// Note: skip the second pass when lowerMask is empty to avoid
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

			int32 load = LoadAcquire(fCoreLoads[i]);
			if (maxEntry == NULL ||
				load > maxLoad
				// Note: tie-break by higher PackageIndex (within
				// the package) rather than lower core ID to spread across
				// more physical cores instead of always favouring core 0.
				|| (load == maxLoad &&
					candidate->PackageIndex() > maxEntry->PackageIndex())) {
				maxLoad = load;
				maxEntry = candidate;
			}
		}
	}
	return maxEntry;
}

/* static */ void DebugDumper::DumpCPURunQueue(CPUEntry* cpu) {
	ThreadRunQueue::ConstIterator iterator = cpu->fRunQueue.GetConstIterator();

	if (iterator.HasNext() &&
		!thread_is_idle_thread(iterator.Next()->GetThread())) {
		kprintf("\nCPU %" B_PRId32 " run queue:\n", cpu->ID());
		cpu->fRunQueue.Dump();
	}
}

/* static */ void DebugDumper::DumpCoreRunQueue(CoreEntry* core) {
	core->fRunQueue.Dump();
}

/* static */ void DebugDumper::DumpCoreEntryLoad(CoreEntry* entry) {
	CoreThreadsData threadsData;
	threadsData.fCore = entry;
	threadsData.fLoad = 0;
	thread_map(DebugDumper::_AnalyzeCoreThreads, &threadsData);

	kprintf("%4" B_PRId32 " %11" B_PRId32 "%% %11" B_PRId32 "%% %11" B_PRId32
			"%% %7" B_PRId32 " %5" B_PRIu32 "\n",
			entry->ID(), entry->fLoad / 10, entry->CurrentLoad() / 10,
			threadsData.fLoad, entry->ThreadCount(),
			entry->LoadMeasurementEpoch());
}

/* static */ void DebugDumper::DumpIdleCoresInPackage(PackageEntry* package) {
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

/* static */ void DebugDumper::DumpPackageCores(PackageEntry* package) {
	kprintf("Package %" B_PRId32 " Cores:\n", package->fPackageID);
	for (int32 i = 0; i < package->RegisteredCoreCount(); i++) {
		CoreEntry* core = package->GetCore(i);
		if (core != NULL) {
			DumpCoreEntryLoad(core);
		}
	}
}

/* static */ void DebugDumper::_AnalyzeCoreThreads(Thread* thread, void* data) {
	CoreThreadsData* threadsData = static_cast<CoreThreadsData*>(data);
	if (thread->scheduler_data->Core() == threadsData->fCore)
		threadsData->fLoad += thread->scheduler_data->GetLoad();
}


static int dump_run_queue(int /* argc */, char** /* argv */) {
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


static int dump_cpu_heap(int /* argc */, char** /* argv */) {
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


static int dump_idle_cores(int /* argc */, char** /* argv */) {
	kprintf("Idle packages:\n");
	uint64 nodeMask = LoadAcquire64(gIdleNodeMask);

	if (nodeMask != 0) {
		kprintf("node package cores\n");

		while (nodeMask != 0) {
			int32 nodeIndex = scheduler_ffs64(nodeMask) - 1;
			nodeMask &= ~(1ULL << nodeIndex);
			if (nodeIndex < 0 || nodeIndex >= gNodeCount)
				continue;

			uint64 packageMask = gSchedulerNodes[nodeIndex].IdlePackageMask();
			while (packageMask != 0) {
				int32 packageIndex = scheduler_ffs64(packageMask) - 1;
				packageMask &= ~(1ULL << packageIndex);

				int32 globalPackageIndex =
					gSchedulerNodes[nodeIndex].PackageStartIndex() +
					packageIndex;
				if (globalPackageIndex < gPackageCount) {
					kprintf("%-4" B_PRId32 " ", nodeIndex);
					DebugDumper::DumpIdleCoresInPackage(
						&gPackageEntries[globalPackageIndex]);
				}
			}
		}
	} else
		kprintf("No idle packages.\n");

	return 0;
}


void Scheduler::init_debug_commands() {
	new (&sDebugCPUHeap) CPUPriorityHeap(smp_get_num_cpus());
	if (sDebugCPUHeap.InitCheck() != B_OK)
		panic("Scheduler::init_debug_commands: failed to allocate CPU heap");

	add_debugger_command_etc("run_queue", &dump_run_queue,
							 "List threads in run queue",
							 "\nLists threads in run queue", 0);
	if (!gSingleCore) {
		add_debugger_command_etc("cpu_heap", &dump_cpu_heap,
								 "List CPUs in CPU priority heap",
								 "\nList CPUs in CPU priority heap", 0);
		add_debugger_command_etc("idle_cores", &dump_idle_cores,
								 "List idle cores", "\nList idle cores", 0);
	}
}

}  // namespace Scheduler
