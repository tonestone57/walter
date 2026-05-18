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

#include <AutoDeleter.h>
#include <DPC.h>
#include <OS.h>
#include <cpu.h>
#include <debug.h>
#include <interrupts.h>
#include <kernel.h>
#include <kscheduler.h>
#include <listeners.h>
#include <load_tracking.h>
#include <safemode.h>
#include <scheduler_defs.h>
#include <slab/Slab.h>
#include <smp.h>
#include <timer.h>
#include <util/Random.h>

#include <algorithm>

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
	void operator()(ThreadData* thread);
};

scheduler_mode Scheduler::sCurrentModeID;
scheduler_mode_operations* Scheduler::sCurrentMode;

bool gSingleCore;
bool gTrackCoreLoad;
bool gTrackCPULoad;
int32 gRandomSamples;

int64 gDeadlineBucketSize __attribute__((aligned(8))) = 5000000;

CoreType gMinCoreType = CORE_TYPE_UNKNOWN;
CoreType gMaxCoreType = CORE_TYPE_UNKNOWN;

bool gHasStandardCores = false;

int32 gTotalRunnableThreads = 0;
uint64 gIdleMask __attribute__((aligned(8))) = 0;

spinlock gSchedulerLock = B_SPINLOCK_INITIALIZER;

int64 gRCUGeneration __attribute__((aligned(8))) = 1;
spinlock gSchedulerUpdateLock = B_SPINLOCK_INITIALIZER;

void scheduler_synchronize() {
	SCHEDULER_ENTER_FUNCTION();

	int32 thisCPU = smp_get_current_cpu();

	// Increment the global generation counter.  All future reschedules
	// will observe the new value.
	int64 targetGen = AddAcquireRelease64(gRCUGeneration, 1) +
					   1;

	// Update current CPU generation to match, so future synchronizations
	// don't wait for us unnecessarily, and so that we don't deadlock if another
	// CPU is also waiting for us in scheduler_synchronize().
	StoreRelease64(CPUEntry::GetCPU(thisCPU)->fRCULastGeneration,
				 targetGen);

	// Broadcast an ICI to all other enabled CPUs to force them into a quiescent
	// state (reschedule).
	int32 cpuCount = smp_get_num_cpus();
	for (int32 i = 0; i < cpuCount; i++) {
		if (i == thisCPU || gCPU[i].disabled)
			continue;
		smp_send_ici(i, SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
	}

	// Wait until all OTHER enabled CPUs have reported a generation >= targetGen.
	for (int32 i = 0; i < cpuCount; i++) {
		if (i == thisCPU || gCPU[i].disabled)
			continue;

		CPUEntry* cpu = CPUEntry::GetCPU(i);
		while (LoadAcquire64(cpu->fRCULastGeneration) < targetGen) {
			if (gCPU[i].disabled)
				break;
			cpu_pause();
		}
	}
}


static timer sInteractionTimer;
static int64 sLastInteractionTime __attribute__((aligned(8)));
static int32 sDPCPending __attribute__((aligned(8))) = 0;
// Atomic guard for sInteractionTimer arming.
// timer_is_active() followed by add_timer() is not atomic: two CPUs can both
// observe the timer as inactive and call add_timer() concurrently, corrupting
// the shared timer_entry.  This flag serialises the arm with a compare-and-set
// so exactly one CPU wins the race.  The flag is cleared by the timer callback
// before it fires the DPC, allowing future re-arming.
static int32 sTimerArmed __attribute__((aligned(8))) = 0;
static int32 sPendingDPCTarget __attribute__((aligned(8))) = 0;	 // 1000 or 5000

// --- Safe snapshot for load balancing decisions ---
static SchedulerSnapshot TakeSnapshot() {
	return MakeSchedulerSnapshot(gTotalRunnableThreads, gIdleMask);
}


static const int kLoadBalanceThreshold = 2;
static const bigtime_t kRescheduleCooldown = 500;

extern "C" void AcquireSchedulerSpinlock() {
	acquire_spinlock(&gSchedulerLock);
}


extern "C" void ReleaseSchedulerSpinlock() {
	release_spinlock(&gSchedulerLock);
}


static void UpdateDeadlineScalingScalable() {
	ThreadData::ComputeQuantumLengths();
}


static void update_quantum_lengths_dpc(void* /*arg*/) {
	while (true) {
		// Note: we can skip the big scheduler lock (and the expensive RCU
		// synchronization in its destructor) if the deadline bucket size
		// is already at the requested target.
		int64 targetResolution = (int64)LoadAcquire(sPendingDPCTarget);
		if (LoadAcquire64(gDeadlineBucketSize) != targetResolution) {
			InterruptsBigSchedulerLocker locker;
			// Re-check target resolution after lock acquisition as it may
			// have changed.
			targetResolution = (int64)LoadAcquire(sPendingDPCTarget);
			if (LoadAcquire64(gDeadlineBucketSize) != targetResolution) {
				StoreRelease64(gDeadlineBucketSize, targetResolution);
				UpdateDeadlineScalingScalable();
			}
		}

		// Atomically clear sDPCPending and re-check sPendingDPCTarget.
		// If a new request arrived while we were processing the last one
		// or synchronizing, we loop back to process it.  This ensures no
		// requests are lost in the window between the loop start and the
		// sDPCPending clear.
		StoreRelease(sDPCPending, 0);
		if ((int64)LoadAcquire(sPendingDPCTarget) == targetResolution)
			break;

		if (GetAndSet(sDPCPending, 1) != 0) {
			// Another CPU already queued a new DPC; we are done.
			break;
		}
	}
}


static status_t interaction_timer_hook(struct timer* timer) {
	// Note: the timer callback must clear sTimerArmed BEFORE
	// attempting to queue the DPC, not after. The previous code cleared
	// sTimerArmed after the DPCQueue::Add call. In the window between Add
	// returning and sTimerArmed being cleared, another CPU executing
	// scheduler_update_interaction_state could see sTimerArmed==1, skip
	// arming, and then the callback clears it - leaving no armed timer and
	// no pending DPC for the next interaction cycle.
	//
	// By clearing sTimerArmed first we allow re-arming immediately if needed,
	// and the DPC guard (sDPCPending) prevents duplicate DPC enqueueing.
	StoreRelease(sTimerArmed, 0);

	StoreRelease(sPendingDPCTarget, 5000000);
	if (GetAndSet(sDPCPending, 1) == 0) {
		int64 target = (int64)LoadAcquire(sPendingDPCTarget);
		if (DPCQueue::DefaultQueue(B_URGENT_DISPLAY_PRIORITY)
				->Add(&update_quantum_lengths_dpc, (void*)(addr_t)target) !=
			B_OK) {
			StoreRelease(sDPCPending, 0);
			StoreRelease(sPendingDPCTarget, 0);
			// DPC queue full; sTimerArmed already cleared above so the
			// next interaction event can re-arm the timer.
		}
		// On success: DPC is in flight; sDPCPending cleared by DPC handler.
	}
	// If sDPCPending was already 1: a DPC is already queued, which will
	// service sPendingDPCTarget. sTimerArmed already cleared above.

	return B_HANDLED_INTERRUPT;
}


void scheduler_update_interaction_state(bigtime_t now) {
	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());

	if (cpu->fInteractionUpdateCounter++ % 32 != 0) {
		// fInteractionUpdateCounter is a
		// plain uint32 incremented without atomics.  This is safe because:
		// (a) it is a per-CPU field - only the current CPU accesses it here
		//     (cpu == GetCPU(smp_get_current_cpu())), and
		// (b) it is purely a throttle counter; a torn or missed increment
		//     merely shifts the phase of the 32-call window, which is
		//     harmless.  No code change required.
		return;
	}

	// Note: Deadline bucket caching. Cache gDeadlineBucketSize once - it is
	// read twice below and the two reads could observe different values if a
	// concurrent DPC is updating it.  A single cached read is also cheaper on
	// the hot path.
	int64 currentBucketSize = LoadAcquire64(gDeadlineBucketSize);

	if (now == 0)
		now = system_time();

	bigtime_t lastTime = (bigtime_t)LoadAcquire64(sLastInteractionTime);
	// Note: Scheduler::MinimalQuantum() reads sCurrentMode->minimal_quantum
	// in two separate memory accesses (pointer load + field read). On 32-bit
	// targets, a concurrent mode switch can change sCurrentMode between these,
	// producing a garbage threshold. Cache the mode pointer first.
	// On 64-bit this is safe (pointer load is atomic) but we document it
	// anyway.
	scheduler_mode_operations* const snapMode = Scheduler::GetCurrentMode();
	bigtime_t threshold = (snapMode != NULL)
							  ? snapMode->minimal_quantum
							  : 1200;  // fallback: 1.2ms minimal quantum

	while (now - lastTime >= threshold) {
		if ((bigtime_t)TestAndSet64(sLastInteractionTime, (int64)now,
				(int64)lastTime) == lastTime) {
			lastTime = now;
			break;
		}

		lastTime = (bigtime_t)LoadAcquire64(sLastInteractionTime);
		if (now - lastTime < threshold)
			return;
	}

	if (currentBucketSize == 1000000) {
		// Replace non-atomic timer_is_active()+add_timer() pair
		// with an atomic test-and-set so only one CPU arms the timer.
		if (GetAndSet(sTimerArmed, 1) == 0) {
			add_timer(&sInteractionTimer, &interaction_timer_hook, 500000,
					  B_ONE_SHOT_RELATIVE_TIMER);
		}
		return;
	}

	// This part is called rarely (only when scaling up resolution)
	// We must not hold scheduler locks here!
	// scheduler_update_interaction_state is called from Enqueue, which HOLDS
	// scheduler locks.
	// Note: (scale-up path): if DPCQueue::Add fails, clear sTimerArmed
	// so that future interactions can still arm the timer.  The previous code
	// set sTimerArmed=1 unconditionally after potentially failing DPC addition,
	// permanently blocking future timer arming.
	// Note: removed the atomic-set(&sDPCPending, 0) that preceded the
	// atomic-get-and-set below.  Clearing sDPCPending unconditionally before
	// the CAS wiped a concurrent CPU's already-queued flag, allowing both CPUs
	// to satisfy the "old == 0" check and enqueue duplicate DPCs.
	StoreRelease(sPendingDPCTarget, 1000000);
	if (GetAndSet(sDPCPending, 1) == 0) {
		int64 target = (int64)LoadAcquire(sPendingDPCTarget);
		if (DPCQueue::DefaultQueue(B_URGENT_DISPLAY_PRIORITY)
				->Add(&update_quantum_lengths_dpc, (void*)(addr_t)target) !=
			B_OK) {
			StoreRelease(sDPCPending, 0);
			StoreRelease(sPendingDPCTarget, 0);
			// Note: when DPC queue is full, ensure sTimerArmed is
			// also cleared so the next interaction event can re-arm the timer.
			// Without this, sTimerArmed stays 0 (it was never set in this
			// path) but the timer is not armed, so gDeadlineBucketSize stays
			// at the wrong resolution until the next DPC queue drain.
			// sTimerArmed is set below; if Add() fails we must NOT set it.
			return;
		}
	}

	// Only arm the timer if the DPC was successfully queued above.
	// Note: sTimerArmed must not be set if Add() failed, since we
	// returned early above in that case and never reach this line.
	if (GetAndSet(sTimerArmed, 1) == 0) {
		add_timer(&sInteractionTimer, &interaction_timer_hook, 500000,
				  B_ONE_SHOT_RELATIVE_TIMER);
	}
}

struct RunQueueScanner {
	uint32 kTopWordMask;
	int kMaxPrioritiesToCheckPerQueue;
	bigtime_t now;

	RunQueueScanner(uint32 topWordMask, int maxPriorities, bigtime_t now)
		: kTopWordMask(topWordMask),
		  kMaxPrioritiesToCheckPerQueue(maxPriorities),
		  now(now) {}

	void operator()(const ThreadRunQueue* runQueue) const {
		if (kMaxPrioritiesToCheckPerQueue <= 0)
			return;

		const uint32* bitmap = runQueue->GetBitmap();
		int checked = 0;

		for (int i = ThreadRunQueue::kBitmapSize - 1; i >= 0; i--) {
			uint32 val = bitmap[i];

			if (i == ThreadRunQueue::kBitmapSize - 1)
				val &= kTopWordMask;

			if (val == 0)
				continue;

			int bit = fls(val) - 1;
			while (true) {
				unsigned int priority = i * 32 + bit;
				// Note: RunQueue scanner logic optimized for dual heaps.
				// For priority boosting we update the most urgent thread
				// in the requested priority bucket.
				ThreadData* thread = runQueue->GetHead(priority);
				// Count each visited priority slot toward the budget, even if
				// the queue head is transiently NULL (e.g. concurrent dequeue or
				// stale bitmap bit). This keeps scan work strictly bounded.
				if (thread != NULL) {
					thread->_UpdatePriorityBoost(now);
				}
				if (++checked >= kMaxPrioritiesToCheckPerQueue)
					return;

				val &= ~(1U << bit);
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

	int32 GetTopoKey(int32 cpu) const {
		return distinctTopology ? get_topology_id(cpu) : (cpu / 16);
	}

	bool operator()(int32 a, int32 b) const {
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

// Since CPU IDs used internally by the kernel bear no relation to the actual
// CPU topology the following arrays are used to efficiently get the core
// and the package that CPU in question belongs to.
static int32* sCPUToCore;
static int32* sCPUToPackage;

enum topology_validation_mode {
	TOPOLOGY_VALIDATION_STRICT = 0,
	TOPOLOGY_VALIDATION_TOLERANT = 1
};

// Switch #1: strict/tolerant topology handling policy.
// Keep strict by default; tolerant mode degrades malformed mappings by
// disabling affected topology entries instead of failing init().
static topology_validation_mode sTopologyValidationMode =
	TOPOLOGY_VALIDATION_STRICT;

static inline bool topology_validation_is_strict() {
	return sTopologyValidationMode == TOPOLOGY_VALIDATION_STRICT;
}

// Switch #2: centralized topology validation reporting helper.
static status_t topology_validation_error(status_t strictStatus,
										  const char* message) {
	dprintf("scheduler: topology validation: %s\n", message);
	return topology_validation_is_strict() ? strictStatus : B_OK;
}


static int32* sPackageToNode;
static int32* sCPUToCluster = NULL;

static void UpdatePriorityBoostScalable(CoreEntry* core, CPUEntry* cpu,
										bigtime_t now = 0) {
	SCHEDULER_ENTER_FUNCTION();

	if (now == 0)
		now = system_time();

	// Note: Mask computation optimization. This mask is recomputed from a
	// compile-time constant on every reschedule.  Hoist it to a static const so
	// the compiler evaluates it once at startup.
	static const uint32 kTopWordMask =
		(uint32)((2ULL << (THREAD_MAX_SET_PRIORITY % 32)) - 1);

	static_assert(THREAD_MAX_SET_PRIORITY < ThreadRunQueue::kBitmapSize * 32,
				  "THREAD_MAX_SET_PRIORITY exceeds bitmap capacity");

	// Throttle: only run the boost scan every 10 context switches to reduce
	// overhead.
	if (cpu->fRescheduleCount++ % 10 != 0)
		return;

	// Scalable Priority Boosting:
	// Instead of scanning all threads (O(N)), we scan only the heads of
	// priority queues (O(1) relative to thread count).
	// We verify if the longest-waiting thread in each queue is starving.
	// This maintains O(1) complexity regardless of the number of threads.

	// Budget is the number of priority buckets examined per run queue,
	// not the number of threads.
	const int kMaxPrioritiesToCheckPerQueue = 5;

	RunQueueScanner scanRunQueue(kTopWordMask, kMaxPrioritiesToCheckPerQueue,
								 now);

	// Check Core RunQueue first to maintain Core -> CPU lock ordering
	// On an N-way SMT core, all N CPUs previously scanned the shared
	// core run queue every 10 reschedules, contending on CoreRunQueueLocker N×
	// more often than necessary.  Gate the scan with round-robin ownership:
	// only the CPU whose (boost_epoch % cpuCount) matches its modular index
	// within the core performs the scan.
	int32 coreCPUCount = max_c(1, core->CPUCount());

	// Note: Counter wrap-around safety. when fRescheduleCount wraps UINT32_MAX
	// → 0, the post-increment is 0, so (0 % 10 == 0) fires and preCount
	// becomes UINT32_MAX, making boostEpoch ≈ 429M.  This causes all CPUs to
	// satisfy the modular ownership check simultaneously, producing a
	// correlated scan burst. Treat the wrap as a normal epoch boundary by
	// clamping preCount.
	uint32 preCount = cpu->fRescheduleCount - 1;
	// Note: Use a non-zero epoch at wrap boundary to avoid bias.
	if (cpu->fRescheduleCount == 0) {
		preCount =
			THREAD_MAX_SET_PRIORITY;  // Arbitrary but stable non-zero value
	}

	uint32 boostEpoch = preCount / 10;

	// Calculate a dense local index using the fLocalIndices bitmask.
	// This ensures fair round-robin ownership even if there are holes
	// in the assigned local indices.
	native_cpu_mask_t localMask = cpu_mask_get_atomic(&core->fLocalIndices);
	const int32 maskBitCount = (int32)(sizeof(native_cpu_mask_t) * 8);
	// 32/64-bit safety: shifting by the type width is undefined behavior.
	// Clamp the shift domain so this remains valid on both 32-bit and 64-bit.
	int32 localIndex = cpu->fCoreLocalIndex;
	if (localIndex < 0)
		localIndex = 0;
	if (localIndex > maskBitCount)
		localIndex = maskBitCount;

	native_cpu_mask_t lowerMask = 0;
	if (localIndex == maskBitCount)
		lowerMask = localMask;
	else if (localIndex > 0)
		lowerMask = localMask & (((native_cpu_mask_t)1 << localIndex) - 1);

	int32 denseLocalIndex = scheduler_popcount(lowerMask);

	bool ownsCoreQueueScan = ((int32)(boostEpoch % (uint32)coreCPUCount) ==
							  (int32)(denseLocalIndex % (uint32)coreCPUCount));

	// Note: CoreRunQueueLocker(core, false) constructs with
	// alreadyLocked=false and lockIfNotLocked defaulting to false, meaning
	// the locker starts in an unlocked state. If Lock() is subsequently
	// called and succeeds, the destructor correctly unlocks. However if
	// the AutoLocker internal fLocked tracking ever diverges from the
	// actual spinlock state (e.g. partial construction failure), the
	// destructor may double-unlock or fail to unlock.
	// Use explicit Lock()/Unlock() calls instead of the two-argument
	// constructor to make the locking intent unambiguous.

	// Note: The thread-count check was done without the lock; a thread
	// could be removed between the check and lock acquisition, making the
	// locked scan a wasted spinlock round-trip.  Re-check inside the lock.
	if (ownsCoreQueueScan) {
		// Scoped locker: ensure Core and CPU run-queue locks are NEVER
		// held simultaneously in this function to eliminate lock inversion.
		CoreRunQueueLocker coreLocker(core, false, false);
		coreLocker.Lock();
		if (core->CoreRunQueueThreadCount() > 0)
			scanRunQueue(core->RunQueue());
	}

	// Check CPU RunQueue
	if (cpu->ThreadCount() > 0) {
		CPURunQueueLocker cpuLocker(cpu);
		scanRunQueue(cpu->RunQueue());
	}
}


static bool enqueue(Thread* thread, bool newOne, Thread* waker,
					bigtime_t now = 0);

void ThreadEnqueuer::operator()(ThreadData* thread) {
	enqueue(thread->GetThread(), false, NULL);
}


void scheduler_dump_thread_data(Thread* thread) {
	thread->scheduler_data->Dump();
}


static bool enqueue(Thread* thread, bool newOne, Thread* waker, bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (now == 0)
		now = system_time();

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
		// scheduler_on_thread_destroy NULLs scheduler_data before
		// the thread is fully torn down; guard against a concurrent destroy.
		ThreadData* wakerData = waker->scheduler_data;
		CoreEntry* wakerCore = (wakerData != NULL) ? wakerData->Core() : NULL;
		if (wakerCore != NULL && wakerCore->CPUCount() > 0)
			targetCore = wakerCore;
	} else if (threadData->Core() != NULL &&
			   (!newOne || !threadData->HasCacheExpired(now))) {
		CPUSet mask = threadData->GetCPUMask();
		targetCore = threadData->Rebalance(mask, now);
	}

	bool wasRunQueueEmpty = false;
	bool requestPreemption = false;
	bool rescheduleNeeded = false;
	bool updateInteraction = false;

	const int32 kMaxRetries = smp_get_num_cpus() * 2 + 8;
	int32 enqueueAttempts = 0;
	do {
		rescheduleNeeded =
			threadData->ChooseCoreAndCPU(targetCore, targetCPU, now);

		if (targetCPU != NULL && targetCore != NULL) {
			TRACE("enqueueing thread %" B_PRId32 " with priority %" B_PRId32
				  " on CPU %" B_PRId32 " (core %" B_PRId32 ")\n",
				  thread->id, threadPriority, targetCPU->ID(),
				  targetCore->ID());
		}

		if (targetCPU == NULL || targetCore == NULL ||
			!threadData->Enqueue(wasRunQueueEmpty, requestPreemption,
								 updateInteraction, now)) {
			targetCore = NULL;
			targetCPU = NULL;

			if (++enqueueAttempts > kMaxRetries) {
				if (threadData->IsReady() && !threadData->IsIdle()) {
					int32 prev = AddAcquireRelease(gTotalRunnableThreads, -1);
					if (prev <= 0) {
						int32 cur = LoadAcquire(gTotalRunnableThreads);
						for (int32 i = 0; i < 100 && cur < 0; i++) {
							int32 was = cur;
							if (TestAndSet(gTotalRunnableThreads, 0, was) == was)
								break;
							cur = LoadAcquire(gTotalRunnableThreads);
							memory_read_barrier();
						}
					}
				}
				return false;
			}

			if (enqueueAttempts == kMaxRetries) {
				targetCPU = CPUEntry::GetCPU(smp_get_current_cpu());
				targetCore = targetCPU->Core();
				if (targetCore == NULL || gCPU[targetCPU->ID()].disabled) {
					if (threadData->IsReady() && !threadData->IsIdle())
						AddRelease(gTotalRunnableThreads, -1);
					return false;
				}
			}
		} else {
			// Note: DO NOT return here. The original early return
			// made all post-loop code (listener notification and IPI dispatch)
			// permanently unreachable dead code. As a result, no IPI was ever
			// sent to wake a sleeping target CPU, causing indefinite scheduling
			// delays until the target CPU's quantum timer fired naturally.
			// Break out of the loop and fall through to IPI dispatch.
			break;
		}
	} while (true);

	// Reached only on successful enqueue (break above).
	// Note: call scheduler_update_interaction_state() while NOT
	// holding any run-queue locks.
	if (updateInteraction)
		scheduler_update_interaction_state(now);

	if (targetCPU == NULL)
		return false;

	// Note: notify listeners - was unreachable before this fix.
	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue,
							 thread);

	int32 heapPriority = CPUPriorityHeap::GetKey(targetCPU);
	if (threadPriority > heapPriority ||
		(threadPriority == heapPriority && rescheduleNeeded) ||
		wasRunQueueEmpty || requestPreemption) {

		// Note: Dynamic Preemption Granularity.
		// Only trigger IPI if Delta(deadline) > epsilon.
		// A more urgent thread (earlier deadline) only preempts if it is
		// significantly more urgent (Delta > epsilon).
		if (targetCPU->ID() != smp_get_current_cpu()) {
			bigtime_t epsilon = targetCPU->PreemptionThreshold();
			Thread* running = gCPU[targetCPU->ID()].running_thread;
			ThreadData* runningData = (running != NULL) ? running->scheduler_data : NULL;
			if (runningData != NULL && !runningData->IsIdle() && !runningData->IsRealTime()) {
				// EEVDF Preemption: scale real-time epsilon to virtual time.
				// vEpsilon = (epsilon * kFairShareReferenceWeight) / weight
				int64 weight = runningData->GetWeight();
				if (weight <= 0)
					weight = 1;
				bigtime_t vEpsilon = (epsilon * 1000) / weight;

				// Preempt only if runningData->Deadline - threadData->Deadline > vEpsilon
				if (runningData->GetVirtualDeadline() - threadData->GetVirtualDeadline() < vEpsilon) {
					// Not urgent enough to justify preemption; preserve cache warmth.
					return true;
				}
			}
		}

		if (targetCPU->ID() == smp_get_current_cpu()) {
			gCPU[targetCPU->ID()].invoke_scheduler = true;
		} else {
			// Note: this IPI dispatch was unreachable before; now
			// correctly wakes the target CPU when a thread is enqueued.
			if (ShouldReschedule(now,
								 (bigtime_t)LoadAcquire64(targetCPU->fLastReschedule),
								 kRescheduleCooldown)) {
				if (targetCPU->SetReschedulePending()) {
					StoreRelease64(targetCPU->fLastReschedule,
										   (int64)now);
					smp_send_ici(targetCPU->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0,
								 NULL, SMP_MSG_FLAG_ASYNC);
				}
			}
		}
	}
	return true;
}


bool enqueue_safe(Thread* thread, bigtime_t now) {
	// Use the same safety logic as ChooseNextThread retry loop
	// Note: return the result of enqueue() which is more reliable
	// than checking IsEnqueued() if enqueue() gave up.
	if (now == 0)
		now = system_time();
	return enqueue(thread, false, NULL, now);
}

/*! Enqueues the thread into the run queue.
	Note: thread lock must be held when entering this function
*/
void scheduler_enqueue_in_run_queue(Thread* thread) {
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	SchedulerModeLocker _;

	AssertThreadReady(thread);

	TRACE("enqueueing new thread %" B_PRId32 " with static priority %" B_PRId32
		  "\n",
		  thread->id, thread->priority);

	ThreadData* threadData = thread->scheduler_data;
	Thread* waker = thread->waker;
	thread->waker = NULL;

	bigtime_t now = system_time();
	threadData->ResetPriorityBoost(now);
	enqueue(thread, true, waker, now);
}

/*! Sets the priority of a thread.
 */
int32 scheduler_set_thread_priority(Thread* thread, int32 priority) {
	ASSERT(are_interrupts_enabled());

	InterruptsSpinLocker _(thread->scheduler_lock);
	SchedulerModeLocker modeLocker;

	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;
	if (threadData == NULL)
		return thread->priority;

	int32 oldPriority = thread->priority;

	TRACE("changing thread %" B_PRId32 " priority to %" B_PRId32
		  " (old: %" B_PRId32 ", effective: %" B_PRId32 ")\n",
		  thread->id, priority, oldPriority,
		  threadData->GetEffectivePriority());

	bigtime_t now = system_time();
	int64 oldWeight = threadData->GetWeight();

	if (priority == oldPriority)
		return oldPriority;

	if (thread->state != B_THREAD_READY) {
		bool wasRealTime = threadData->IsRealTime();
		thread->priority = priority;
		threadData->ResetPriorityBoost(now);
		bool isRealTime = threadData->IsRealTime();
		int64 newWeight = threadData->GetWeight();

		if (thread->state == B_THREAD_RUNNING) {
			ASSERT(threadData->Core() != NULL);

			ASSERT(thread->cpu != NULL);
			CPUEntry* cpu = &gCPUEntries[thread->cpu->cpu_num];

			if (!gCPU[cpu->ID()].disabled) {
				CoreCPUHeapLocker _(threadData->Core());
				cpu->UpdatePriority(priority);

				if (!wasRealTime && !isRealTime)
					cpu->AddWeight(newWeight - oldWeight);
				else if (!wasRealTime && isRealTime)
					cpu->AddWeight(-oldWeight);
				else if (wasRealTime && !isRealTime)
					cpu->AddWeight(newWeight);
			}
		}

		return oldPriority;
	}

	// The thread is in the run queue. We need to remove it and re-insert it at
	// a new position.

	T(RemoveThread(thread));

	// notify listeners
	NotifySchedulerListeners(&SchedulerListener::ThreadRemovedFromRunQueue,
							 thread);

	// Dequeue while threadData->fWeight still holds the old weight.
	// This ensures symmetric accounting in CPUEntry::Remove.
	bool enqueued = threadData->Dequeue();

	thread->priority = priority;
	threadData->ResetPriorityBoost(now);

	if (enqueued)
		enqueue(thread, true, NULL, now);

	return oldPriority;
}


void scheduler_reschedule_ici() {
	// This function is called as a result of an incoming ICI.
	// Make sure the reschedule() is invoked.
	get_cpu_struct()->invoke_scheduler = true;
}


static inline void stop_cpu_timers(Thread* fromThread, Thread* toThread) {
	SpinLocker teamLocker(&fromThread->team->time_lock);
	SpinLocker threadLocker(&fromThread->time_lock);

	if (fromThread->HasActiveCPUTimeUserTimers() ||
		fromThread->team->HasActiveCPUTimeUserTimers()) {
		user_timer_stop_cpu_timers(fromThread, toThread);
	}
}


static inline void continue_cpu_timers(Thread* thread, cpu_ent* cpu) {
	SpinLocker teamLocker(&thread->team->time_lock);
	SpinLocker threadLocker(&thread->time_lock);

	if (thread->HasActiveCPUTimeUserTimers() ||
		thread->team->HasActiveCPUTimeUserTimers()) {
		user_timer_continue_cpu_timers(thread, cpu->previous_thread);
	}
}


static void thread_resumes(Thread* thread) {
	cpu_ent* cpu = thread->cpu;
	Thread* previousThread = cpu->previous_thread;

	// continue CPU time based user timers
	// Note: continue timers while still holding the previous thread's
	// scheduler lock.  The undertaker thread (on another CPU) waits for
	// this lock before freeing the thread; if we release it before calling
	// continue_cpu_timers (which dereferences cpu->previous_thread) we
	// risk a use-after-free.
	continue_cpu_timers(thread, cpu);

	release_spinlock(&previousThread->scheduler_lock);

	// notify the user debugger code
	if ((thread->flags & THREAD_FLAGS_DEBUGGER_INSTALLED) != 0)
		user_debug_thread_scheduled(thread);
}


void scheduler_new_thread_entry(Thread* thread) {
	thread_resumes(thread);

	SpinLocker locker(thread->time_lock);
	thread->last_time = system_time();
}

/*! Switches the currently running thread.
	This is a service function for scheduler implementations.

	\param fromThread The currently running thread.
	\param toThread The thread to switch to. Must be different from
		\a fromThread.
*/
static inline void switch_thread(Thread* fromThread, Thread* toThread) {
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


static void reschedule(int32 nextState) {
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	int32 thisCPU = smp_get_current_cpu();
	CPUEntry* cpu = CPUEntry::GetCPU(thisCPU);

	// RCU Quiescent State: Report current generation.
	// Since reschedule() runs with interrupts disabled, it forms a
	// natural RCU critical section boundary.
	StoreRelease64(cpu->fRCULastGeneration,
				 LoadAcquire64(gRCUGeneration));

	gCPU[thisCPU].invoke_scheduler = false;

	bigtime_t now = system_time();

	cpu->ClearReschedulePending();
	CoreEntry* core = CoreEntry::GetCore(thisCPU);

	Thread* oldThread = thread_get_current_thread();
	ThreadData* oldThreadData = oldThread->scheduler_data;

	CPUSet oldThreadMask;
	bool useOldThreadMask, fetchedOldThreadMask = false;

	oldThreadData->StopCPUTime(now);

	SchedulerModeLocker modeLocker;

	TRACE("reschedule(): cpu %" B_PRId32 ", current thread = %" B_PRId32 "\n",
		  thisCPU, oldThread->id);

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

			if (!oldThreadData->IsIdle() &&
				(!useOldThreadMask || oldThreadMask.GetBit(thisCPU))) {
				oldThreadData->Continues(now);
				if (oldThreadData->HasQuantumEnded(oldThread->cpu->preempted,
												   oldThread->has_yielded,
												   now)) {
					TRACE(
						"enqueueing thread %ld into run queue priority ="
						" %ld\n",
						oldThread->id, oldThreadData->GetEffectivePriority());
					putOldThreadAtBack = true;
				} else {
					TRACE(
						"putting thread %ld back in run queue priority ="
						" %ld\n",
						oldThread->id, oldThreadData->GetEffectivePriority());
					putOldThreadAtBack = false;
				}
			}

			break;
		case THREAD_STATE_FREE_ON_RESCHED:
			oldThreadData->Dies(now);
			// Note: a dying thread must NEVER be re-enqueued. Clear
			// enqueueOldThread unconditionally here. Without this, if the
			// mask-based migration path below sets enqueueOldThread=false
			// correctly, the code falls through fine; but if the switch cases
			// are reordered or an early-exit is added, the dying thread
			// could be enqueued via enqueue(oldThread, true, NULL) after
			// Dies() has already removed its load, corrupting
			// gTotalRunnableThreads and leading to use-after-free in the
			// run-queue drain during thread destruction.
			enqueueOldThread = false;
			break;
		default:
			oldThreadData->GoesAway(now);
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
			core->DecrementTotalThreadCount();
			// Note: track activity for the last quantum before disable.
			cpu->UpdateActiveTime(oldThreadData, now);

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
		bool oldThreadShouldMigrate =
			useOldThreadMask && !oldThreadMask.GetBit(thisCPU);
		if (oldThreadShouldMigrate)
			enqueueOldThread = false;

		// Note: Advance System Virtual Time and check eligibility before selection.
		cpu->CheckEligibility(cpu->SystemVirtualTime());

		nextThreadData = cpu->ChooseNextThread(
			enqueueOldThread ? oldThreadData : NULL, putOldThreadAtBack, now);

		if (nextThreadData == NULL) {
			nextThreadData = cpu->PeekIdleThread();
			if (nextThreadData == NULL)
				nextThreadData = oldThreadData;
		}

		cpu->UpdateActiveTime(oldThreadData, now);

		if (oldThreadShouldMigrate) {
			enqueue(oldThread, true, NULL, now);
			// replace with the idle thread, if no other thread could be found
			if (oldThreadData == nextThreadData) {
				nextThreadData = cpu->PeekIdleThread();
				if (nextThreadData == NULL)
					nextThreadData = oldThreadData;
			}
		}

		// update CPU heap
		CoreCPUHeapLocker cpuLocker(core);
		cpu->UpdatePriority(nextThreadData->GetEffectivePriority());
	}

	UpdatePriorityBoostScalable(core, cpu, now);

	Thread* nextThread = nextThreadData->GetThread();
	ASSERT(!gCPU[thisCPU].disabled || nextThreadData->IsIdle());

	if (nextThread != oldThread) {
		if (enqueueOldThread) {
			if (putOldThreadAtBack)
				enqueue(oldThread, false, NULL, now);
			else
				oldThreadData->PutBack(now);
		}

		acquire_spinlock(&nextThread->scheduler_lock);
	}

	TRACE("reschedule(): cpu %" B_PRId32 ", next thread = %" B_PRId32 "\n",
		  thisCPU, nextThread->id);

	T(ScheduleThread(nextThread, oldThread));

	// notify listeners
	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled, oldThread,
							 nextThread);

	ASSERT(nextThreadData->Core() == core);
	nextThread->state = B_THREAD_RUNNING;
	nextThreadData->StartCPUTime(now);

	// track CPU activity
	cpu->TrackLoad(nextThreadData, now);

	if (nextThread != oldThread || oldThread->cpu->preempted) {
		// Dynamic Quantum Scaling:
		// Reduce quantum if the core is crowded to maintain interactivity.
		// Note: Transient counter state safety. ThreadCount() can transiently
		// return 0 during a remove race.  If load == 1, (load - 1) == 0 causes
		// division by zero. Clamp divisor to at least 1.
		int32 load = core->ThreadCount();
		bigtime_t quantum = Scheduler::BaseQuantum();
		if (load > 2) {
			int32 divisor = max_c(1, load - 1);
			quantum = max_c(Scheduler::MinimalQuantum(), quantum / divisor);
		}
		nextThreadData->SetQuantum(quantum);

		// Note: Dynamic Preemption Granularity.
		// Delta(deadline) > epsilon check would go here if we were implementing
		// preemption in the middle of a quantum.

		cpu->StartQuantumTimer(nextThreadData, oldThread->cpu->preempted);

		oldThread->cpu->preempted = false;
		if (!nextThreadData->IsIdle())
			nextThreadData->Continues(now);
		else
			Scheduler::RebalanceIRQs(true);
		nextThreadData->StartQuantum(now);

		modeLocker.Unlock();

		SCHEDULER_EXIT_FUNCTION();

		if (nextThread != oldThread)
			switch_thread(oldThread, nextThread);
	}
}

/*! Runs the scheduler.
	Note: expects thread spinlock to be held
*/
void scheduler_reschedule(int32 nextState) {
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	if (!LoadAcquire(sSchedulerEnabled)) {
		Thread* thread = thread_get_current_thread();
		if (thread != NULL && nextState != B_THREAD_READY)
			panic("scheduler_reschedule_no_op() called in non-ready thread");
		return;
	}

	reschedule(nextState);
}


status_t scheduler_on_thread_create(Thread* thread, bool idleThread) {
	void* buffer = object_cache_alloc(sThreadDataCache, 0);
	if (buffer == NULL)
		return B_NO_MEMORY;

	thread->scheduler_data = new (buffer) ThreadData(thread);
	return B_OK;
}


void scheduler_on_thread_init(Thread* thread) {
	ASSERT(thread->scheduler_data != NULL);

	if (thread_is_idle_thread(thread)) {
		static int32 sIdleThreadsID __attribute__((aligned(8)));
		int32 cpuID = AddAcquireRelease(sIdleThreadsID, 1);

		thread->previous_cpu = &gCPU[cpuID];
		thread->pinned_to_cpu = 1;

		thread->scheduler_data->Init(CoreEntry::GetCore(cpuID));
	} else
		thread->scheduler_data->Init();
}


void scheduler_on_thread_destroy(Thread* thread) {
	ThreadData* threadData;
	{
		InterruptsSpinLocker locker(thread->scheduler_lock);
		threadData = thread->scheduler_data;
		thread->scheduler_data = NULL;
	}

	if (threadData != NULL) {
		threadData->~ThreadData();
		object_cache_free(sThreadDataCache, threadData);
	}
}

/*! This starts the scheduler. Must be run in the context of the initial idle
	thread. Interrupts must be disabled and will be disabled when returning.
*/
void scheduler_start() {
	InterruptsSpinLocker _(thread_get_current_thread()->scheduler_lock);
	SCHEDULER_ENTER_FUNCTION();

	reschedule(B_THREAD_READY);
}


status_t scheduler_set_operation_mode(scheduler_mode mode) {
	if (mode != SCHEDULER_MODE_LOW_LATENCY &&
		mode != SCHEDULER_MODE_POWER_SAVING) {
		return B_BAD_VALUE;
	}

	dprintf("scheduler: switching to %s mode\n", sSchedulerModes[mode]->name);

	{
		InterruptsBigSchedulerLocker _;

		Scheduler::SetOperationMode(mode, sSchedulerModes[mode]);
		Scheduler::SwitchToMode();

		ThreadData::ComputeQuantumLengths();
	}
	scheduler_synchronize();

	return B_OK;
}


void scheduler_set_cpu_enabled(int32 cpuID, bool enabled) {
#if KDEBUG
	if (are_interrupts_enabled())
		panic("scheduler_set_cpu_enabled: called with interrupts enabled");
#endif

	dprintf("scheduler: %s CPU %" B_PRId32 "\n",
			enabled ? "enabling" : "disabling", cpuID);

	if (cpuID < 0 || cpuID >= smp_get_num_cpus()) {
		dprintf("scheduler: ignoring %s request for invalid CPU %" B_PRId32
				" (valid range: 0..%" B_PRId32 ")\n",
				enabled ? "enable" : "disable", cpuID, smp_get_num_cpus() - 1);
		return;
	}

	Scheduler::SetCPUEnabled(cpuID, enabled);

	CPUEntry* cpu = &gCPUEntries[cpuID];
	CoreEntry* core = cpu->Core();

	ASSERT(core->CPUCount() >= 0);

	if (enabled) {
		// serialize AddCPU with the same global scheduler lock scope used by
		// the disable/remove path to avoid races with idle-core state updates.
		{
			InterruptsBigSchedulerLocker bigLocker;
			CoreCPUHeapLocker heapLocker(core);

			// Note: AddCPU inserts the CPU into the heap. A concurrent
			// enqueue() that races between disabled=false and the heap insert
			// can call UpdatePriority on a CPU with no heap link, panicking.
			// Fix: complete AddCPU FIRST while disabled is still true, then
			// clear the disabled flag and publish the CPU via gCPUEnabled.
			core->AddCPU(cpu);
			// Note: set disabled=false and SetBitAtomic atomically
			// under CoreCPUHeapLocker. GetCPUMask reads gCPUEnabled; enqueue
			// checks !gCPU[id].disabled. Both must agree simultaneously.
			gCPU[cpuID].disabled = false;
			gCPUEnabled.SetBitAtomic(cpuID);
		}

		// Start the CPU after publishing all scheduler state and after
		// releasing scheduler/core locks to avoid lock-order inversions if the
		// startup path enters the scheduler immediately.
		cpu->Start();

		// RCU Update: Wait for all CPUs to see the new CPU.
		scheduler_synchronize();
	} else {
		// Improve serialization of queue migration/removal:
		// hold the global scheduler lock scope so concurrent scheduling on
		// other CPUs cannot observe partial migration states.
		bool sendRescheduleICI = false;
		{
			InterruptsBigSchedulerLocker bigLocker;

			gCPU[cpuID].disabled = true;
			gCPUEnabled.ClearBitAtomic(cpuID);

			// If this is the last CPU in the core, we need to unassign threads
			// from the core.  We do this AFTER marking the CPU disabled and
			// acquiring the scheduler lock. This ensures no new threads are
			// assigned to the core while we are unassigning them.
			if (core->CPUCount() == 1)
				thread_map(CoreEntry::_UnassignThread, core);

			ThreadEnqueuer enqueuer;

			// flush CPU run queue
			while (true) {
				// The flush loop holds CPURunQueueLocker per-iteration but not
				// CoreRunQueueLocker. Global serialization comes from
				// InterruptsBigSchedulerLocker held for this disable scope.
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
				// Note: document lock ordering.
				// CoreCPUHeapLocker acquires fCPULock. RemoveCPU internally
				// calls fPackage->RemoveIdleCore() which acquires fCoreLock
				// (write). Lock ordering: fCPULock → fCoreLock. Verify no other
				// path acquires these in reverse order. AddIdleCore() acquires
				// fCoreLock then is called from AddCPU under fCPULock - same
				// ordering, safe. CoreGoesIdle calls PackageEntry::CoreGoesIdle
				// (no fCoreLock) - no ordering conflict.
				core->RemoveCPU(cpu, enqueuer);
			}

			cpu->Stop();
			sendRescheduleICI = smp_get_current_cpu() != cpuID;
		}
		// RCU Update: Wait for all CPUs to see that the CPU is disabled.
		scheduler_synchronize();

		// don't wait until the thread quantum ends
		if (sendRescheduleICI)
			smp_send_ici(cpuID, SMP_MSG_RESCHEDULE, 0, 0, 0, NULL,
						 SMP_MSG_FLAG_ASYNC);
	}
}


static void traverse_topology_tree(const cpu_topology_node* node, int packageID,
								   int coreID, int32& coreIndex,
								   int32 cpuCount) {
	switch (node->level) {
		case CPU_TOPOLOGY_SMT: {
			bool nodeValid = node->id < cpuCount;
			bool coreValid = coreID < cpuCount;

			if (!nodeValid) {
				dprintf(
					"scheduler: topology node id %d out of bounds (max "
					"%" B_PRId32 ")\n",
					node->id, cpuCount);
			}
			if (!coreValid) {
				dprintf("scheduler: core index %d out of bounds (max %" B_PRId32
						")\n",
						coreID, cpuCount);
			}

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


static int32 get_topology_id(int32 cpuID) {
	// Note: the original code evaluated cache_id[gCPUCacheLevelCount-1]
	// before the branch. When gCPUCacheLevelCount==0, the subscript -1 is
	// out-of-bounds UB even though the result is never used (branch not taken).
	// Rewrite to avoid any evaluation when count==0.
	if (gCPUCacheLevelCount <= 0)
		return sCPUToPackage[cpuID];
	return gCPU[cpuID].cache_id[gCPUCacheLevelCount - 1];
}


static status_t build_topology_mappings(int32& cpuCount, int32& coreCount,
										int32& packageCount, int32& nodeCount) {
	cpuCount = smp_get_num_cpus();
	coreCount = 0;
	packageCount = 0;
	nodeCount = 0;

	delete[] sCPUToCore;
	delete[] sCPUToCluster;
	delete[] sPackageToNode;
	delete[] sCPUToPackage;

	sCPUToCore = new (std::nothrow) int32[cpuCount];
	sCPUToCluster = new (std::nothrow) int32[cpuCount];
	// Allocate cpuCount + 1 elements: in the degenerate case of one core per
	// package the final packageCount equals cpuCount and the last package's
	// node mapping is written at index packageCount before the post-loop
	// increment.  The extra element prevents a potential one-past-the-end
	// write on systems where the topology detection produces packageCount ==
	// cpuCount entries.
	sPackageToNode = new (std::nothrow) int32[cpuCount + 1];
	sCPUToPackage = new (std::nothrow) int32[cpuCount];

	if (sCPUToCore == NULL || sCPUToCluster == NULL || sPackageToNode == NULL ||
		sCPUToPackage == NULL) {
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

	// Safe upper bound allocation for mapping packages to nodes
	ArrayDeleter<int32> packageToNodeDeleter(sPackageToNode);

	// sPackageToNode is the only mapping array not zero-initialized.
	// Packages that are never written (guard short-circuits) carry heap
	// garbage, which init() then uses as a node index, mapping the package to a
	// non-existent SchedulerNode.
	memset(sPackageToNode, 0, sizeof(int32) * (cpuCount + 1));

	// First pass: logical topology from ACPI/Device Tree
	const cpu_topology_node* root = get_cpu_topology();
	int32 coreIndex = 0;

	if (root != NULL)
		traverse_topology_tree(root, 0, 0, coreIndex, cpuCount);
	else {
		// Fallback for missing topology: 1-to-1 mapping
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

	// Second pass: Topology-Aware Clustering
	// Cluster Strategy:
	// 1. Group CPUs by L3 Cache ID (Topology Domain).
	// 2. Map each L3 domain to a unique SchedulerNode.
	// 3. Within each L3 domain, split cores into Packages (Clusters) of target
	// size 4.
	// 4. Balance "runt" clusters (5-7 cores) evenly (e.g., 6 -> 3+3, not 4+2).

	int32* cpuList = new (std::nothrow) int32[cpuCount];
	if (cpuList == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> cpuListDeleter(cpuList);

	for (int32 i = 0; i < cpuCount; i++) cpuList[i] = i;

	// Fallback check: If all CPUs report Topology ID 0 (detection failed),
	// create virtual L3 domains of 16 cores to avoid putting everything in one
	// massive Node.
	bool distinctTopology = false;
	int32 firstTopo = get_topology_id(0);
	for (int32 i = 1; i < cpuCount; i++) {
		if (get_topology_id(i) != firstTopo) {
			distinctTopology = true;
			break;
		}
	}

	// Sort by L3 Topology ID
	TopologyComparator comparator(distinctTopology);

	std::sort(cpuList, cpuList + cpuCount, comparator);

	packageCount = 0;
	nodeCount = 0;

	int32 l3Start = 0;
	while (l3Start < cpuCount) {
		int32 topologyID = distinctTopology ? get_topology_id(cpuList[l3Start])
											: (cpuList[l3Start] / 16);
		int32 l3End = l3Start + 1;

		// Find end of current L3 domain
		while (l3End < cpuCount) {
			int32 nextTopo = distinctTopology ? get_topology_id(cpuList[l3End])
											  : (cpuList[l3End] / 16);
			if (nextTopo != topologyID)
				break;
			l3End++;
		}

		int32 coresInL3 = l3End - l3Start;
		int32 targetClusterSize = 4;

		// Calculate balanced cluster sizes
		// Formula: Round to nearest integer to find ideal cluster count.
		// Example: 13 cores, target 4. 13/4 = 3.25 -> 3 clusters.
		// Distribution: 5, 4, 4 (Low variance).
		int32 numClusters =
			(coresInL3 + targetClusterSize / 2) / targetClusterSize;
		if (numClusters < 1)
			numClusters = 1;

		int32 baseSize = coresInL3 / numClusters;
		int32 remainder = coresInL3 % numClusters;

		int32 currentPackageSize = 0;
		int32 clusterIndex = 0;

		// Create a SchedulerNode for this L3 domain
		int32 currentNodeID = nodeCount++;
		int32 coresInCurrentNode = 0;
		const int32 kMaxCoresPerNode = 16;

		if (coresInL3 > 0) {
			// Note: ensure we don't write past cpuCount.
			if (packageCount < cpuCount)
				sPackageToNode[packageCount] = currentNodeID;

			// Note: when this is the very last package that fits
			// within the cpuCount limit AND we are at the boundary where
			// the inner cluster-split guard (packageCount + 1 < cpuCount)
			// will prevent further increments, the final packageCount++ at
			// the end of the L3 domain will cover the count but the
			// sPackageToNode entry for the *current* packageCount was
			// written above.  Verify the invariant with a debug assertion.
			ASSERT(packageCount < cpuCount + 1);

			for (int32 i = 0; i < coresInL3; i++) {
				int32 cpuID = cpuList[l3Start + i];

				// Sanity check: If a single L3 node gets too large (e.g. bad
				// BIOS reporting entire socket as one L3), split it into
				// pseudo-nodes to reduce lock contention.
				if (coresInCurrentNode >= kMaxCoresPerNode) {
					// Note: pseudo-node IDs can exceed 64.  Remapping
					// occurs in init().
					currentNodeID = nodeCount++;
					coresInCurrentNode = 0;

					if (packageCount < cpuCount)
						sPackageToNode[packageCount] = currentNodeID;
				}

				int32 clusterSize =
					baseSize + (clusterIndex < remainder ? 1 : 0);
				if (currentPackageSize >= clusterSize) {
					// Note: guard uses cpuCount as an upper bound.
					// The sPackageToNode array was allocated with cpuCount+1
					// elements (to handle the one-past-the-end case for the
					// last package increment). Use the tighter bound here to
					// ensure the final packageCount++ stays within the
					// allocated range.
					if (packageCount + 1 <= cpuCount) {
						currentPackageSize = 0;
						clusterIndex++;
						packageCount++;
						sPackageToNode[packageCount] = currentNodeID;
					}
					// Note: Package limit guard documentation. when the guard
					// above prevents a new package from being created, the
					// remaining CPUs in this cluster are folded into the
					// current package.  No sPackageToNode write is needed since
					// the current package's node was already written at cluster
					// start.
				}

				if (packageCount < cpuCount)
					sCPUToCluster[cpuID] = packageCount;

				currentPackageSize++;
				coresInCurrentNode++;
			}

			// Note: increment packageCount to include the last package
			// of the current L3 domain BEFORE breaking due to the CPU limit.
			// Note: the unconditional packageCount++ after the loop
			// can push packageCount to cpuCount+1 before the >=cpuCount clamp.
			// sPackageToNode was allocated with cpuCount+1 elements but a
			// second L3 domain's loop could write at index cpuCount+1 if the
			// first domain reached the limit. Guard strictly.
			if (packageCount < cpuCount) {
				packageCount++;
			}
			if (packageCount >= cpuCount) {
				packageCount = cpuCount;  // clamp
				break;
			}
		}
		l3Start = l3End;
	}

	// (clarification): scheduler_on_team_foreground_changed reads
	// fThread->team->fIsForeground without the team lock in choose_core.
	// This is a single-byte read that is atomic on all supported architectures;
	// a torn read is impossible and the worst outcome is a one-quantum stale
	// placement, which self-corrects on the next rebalance.  No lock change
	// is warranted here.

	cpuToCoreDeleter.Detach();
	cpuToPackageDeleter.Detach();
	cpuToClusterDeleter.Detach();
	packageToNodeDeleter.Detach();
	return B_OK;
}


static status_t init() {
	gIdleNodeMask = 0;

	gMinCoreType = CORE_TYPE_UNKNOWN;
	gMaxCoreType = CORE_TYPE_UNKNOWN;
	gHasStandardCores = false;

	// create logical processor to core and package mappings
	int32 cpuCount, coreCount, packageCount, nodeCount;
	status_t result =
		build_topology_mappings(cpuCount, coreCount, packageCount, nodeCount);
	if (result != B_OK)
		return result;

	// These arrays are only used for initialization and can be freed now.
	ArrayDeleter<int32> cpuToCoreDeleter(sCPUToCore);
	ArrayDeleter<int32> cpuToPackageDeleter(sCPUToPackage);
	ArrayDeleter<int32> cpuToClusterDeleter(sCPUToCluster);
	ArrayDeleter<int32> packageToNodeDeleter(sPackageToNode);

	if (packageCount > 4096) {
		dprintf("scheduler: system has too many packages (%" B_PRId32
				" > 4096). "
				"Limiting to 4096 packages. Excess cores will be disabled.\n",
				packageCount);
		packageCount = 4096;
	}

	// disable parts of the scheduler logic that are not needed
	gSingleCore = coreCount == 1;
	scheduler_update_policy();

	gCoreCount = coreCount;
	gPackageCount = packageCount;

	// Use topology-aware nodes detected by build_topology_mappings.
	// Keep all nodes (do not fold by modulo). If the topology is malformed,
	// fail early rather than clamping, since clamping would invalidate
	// package->node mappings and can cause out-of-bounds accesses.
	if (nodeCount <= 0) {
		if (topology_validation_error(
				B_BAD_DATA,
				"invalid topology node count, forcing single-node fallback") !=
			B_OK) {
			return B_BAD_DATA;
		}
		nodeCount = 1;
	}
	gNodeCount = nodeCount;

	gSchedulerNodes = new (std::nothrow) SchedulerNode[nodeCount];
	if (gSchedulerNodes == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<SchedulerNode> schedulerNodesDeleter(gSchedulerNodes);

	for (int32 i = 0; i < nodeCount; i++) gSchedulerNodes[i].Init(i);

	gCPUEntries = new (std::nothrow) CPUEntry[cpuCount];
	gCoreEntries = new (std::nothrow) CoreEntry[coreCount];
	gPackageEntries = new (std::nothrow) PackageEntry[packageCount];

	if (gCPUEntries == NULL || gCoreEntries == NULL ||
		gPackageEntries == NULL) {
		delete[] gCPUEntries;
		delete[] gCoreEntries;
		delete[] gPackageEntries;
		return B_NO_MEMORY;
	}

	ArrayDeleter<CPUEntry> cpuEntriesDeleter(gCPUEntries);
	ArrayDeleter<CoreEntry> coreEntriesDeleter(gCoreEntries);
	ArrayDeleter<PackageEntry> packageEntriesDeleter(gPackageEntries);

	uint8* seenNode = new (std::nothrow) uint8[nodeCount];
	if (seenNode == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<uint8> seenNodeDeleter(seenNode);
	memset(seenNode, 0, sizeof(uint8) * nodeCount);

	int32 currentNode = -1;
	int32 currentPackageIndexInNode = 0;

	for (int32 i = 0; i < packageCount; i++) {
		int32 nodeIndex = sPackageToNode[i];
		if (nodeIndex < 0 || nodeIndex >= nodeCount) {
			if (topology_validation_error(
					B_BAD_DATA, "invalid package->node mapping") != B_OK) {
				return B_BAD_DATA;
			}
			nodeIndex = 0;
		}

		if (nodeIndex != currentNode) {
			if (seenNode[nodeIndex] != 0) {
				status_t mappingStatus = topology_validation_error(
					B_BAD_DATA, "non-contiguous package->node mapping");
				if (mappingStatus != B_OK) {
					return B_BAD_DATA;
				}
				// Tolerant mode fallback: keep package contiguous by folding it
				// into the currently active node block.
				nodeIndex = currentNode >= 0 ? currentNode : 0;
			}
			if (currentNode != -1)
				gSchedulerNodes[currentNode].SetPackageCount(
					currentPackageIndexInNode);

			seenNode[nodeIndex] = 1;
			currentNode = nodeIndex;
			currentPackageIndexInNode = 0;
			gSchedulerNodes[currentNode].SetPackageStartIndex(i);
		}

		int32 packageIndexInNode = currentPackageIndexInNode;
		const int32 kMaxPackagesPerNode = sizeof(native_cpu_mask_t) * 8;
		if (packageIndexInNode >= kMaxPackagesPerNode ||
			packageIndexInNode < 0) {
			if (packageIndexInNode == kMaxPackagesPerNode) {
				dprintf(
					"scheduler: warning: node %" B_PRId32
					" has more than %d "
					"packages. Excess packages will not have idle tracking.\n",
					nodeIndex, kMaxPackagesPerNode);
			}
			packageIndexInNode = -1;
		}

		gPackageEntries[i].Init(i, &gSchedulerNodes[nodeIndex],
								packageIndexInNode);
		currentPackageIndexInNode++;
	}

	if (currentNode != -1)
		gSchedulerNodes[currentNode].SetPackageCount(currentPackageIndexInNode);

	// Map Core to Package and assign index within package
	int32* packageCoreCounters = new (std::nothrow) int32[packageCount];
	if (packageCoreCounters == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> packageCoreCountersDeleter(packageCoreCounters);
	memset(packageCoreCounters, 0, sizeof(int32) * packageCount);

	// Determine package index for each core
	// We need to iterate cores, but we only have map CPU->Core and CPU->Cluster
	int32* coreToPackage = new (std::nothrow) int32[coreCount];
	if (coreToPackage == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> coreToPackageDeleter(coreToPackage);
	for (int32 i = 0; i < coreCount; i++) coreToPackage[i] = -1;

	for (int32 i = 0; i < cpuCount; i++) {
		int32 coreIndex = sCPUToCore[i];
		int32 packageID = sCPUToCluster[i];
		if (coreIndex < 0 || coreIndex >= coreCount) {
			if (topology_validation_error(
					B_BAD_DATA, "invalid cpu->core mapping") != B_OK) {
				return B_BAD_DATA;
			}
			continue;
		}
		if (packageID < 0) {
			if (topology_validation_error(
					B_BAD_DATA, "invalid cpu->package mapping") != B_OK) {
				return B_BAD_DATA;
			}
			continue;
		}
		if (coreToPackage[coreIndex] != -1 &&
			coreToPackage[coreIndex] != packageID) {
			if (topology_validation_error(
					B_BAD_DATA, "inconsistent core->package mapping") != B_OK) {
				return B_BAD_DATA;
			}
			continue;
		}
		coreToPackage[coreIndex] = packageID;
	}

	for (int32 i = 0; i < coreCount; i++) {
		int32 packageID = coreToPackage[i];
		CoreEntry* core = &gCoreEntries[i];

		if (packageID < 0 || packageID >= packageCount) {
			// This core belongs to a package beyond the limit. Skip
			// initialization.
			continue;
		}

		PackageEntry* package = &gPackageEntries[packageID];
		int32 packageIndex = packageCoreCounters[packageID]++;

		if (packageIndex >= kMaxCoresPerPackage) {
			// Disable excess cores instead of panicking
			dprintf("Scheduler: Package %" B_PRId32
					" has too many cores (%" B_PRId32 " > %" B_PRId32
					"). Disabling core %" B_PRId32 ".\n",
					packageID, packageIndex + 1, kMaxCoresPerPackage, i);

			// We can't easily mark it disabled here as we are iterating cores,
			// not CPUs. But we skip Init(), so Package() remains NULL. The next
			// loop iterates CPUs and checks if Core->Package() is NULL.
			continue;
		}

		core->Init(i, package);
		core->fPackageIndex = packageIndex;
		package->RegisterCore(packageIndex, core);
	}

	for (int32 i = 0; i < cpuCount; i++) {
		int32 coreIndex = sCPUToCore[i];
		if (coreIndex < 0 || coreIndex >= coreCount) {
			status_t mappingStatus = topology_validation_error(
				B_BAD_DATA, "invalid cpu->core mapping during cpu init");
			if (mappingStatus != B_OK)
				return B_BAD_DATA;
			gCPU[i].disabled = true;
			continue;
		}
		CoreEntry* core = &gCoreEntries[coreIndex];

		if (core->Package() == NULL) {
			dprintf("scheduler: disabling cpu %" B_PRId32 " (topology limit)\n",
					i);
			gCPU[i].disabled = true;
			continue;
		}

		gCPUEntries[i].Init(i, core);
		core->AddCPU(&gCPUEntries[i]);
	}

	// Determine CPU Capacities (Heterogeneous Support)
	// We use the CPU frequency as a proxy for performance capacity.
	// detectedHeterogeneous is set to true only when cpu_frequency() returns
	// at least two distinct non-zero values, confirming that the hardware
	// exposes different core performance tiers. It gates the Alder Lake
	// fallback heuristic below: without this flag the heuristic would
	// misclassify any homogeneous system with >8 cores.
	bool detectedHeterogeneous = false;
	uint64 maxFreq = 0;
	uint64* cpuFreqs = new (std::nothrow) uint64[cpuCount];
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
				dprintf(
					"scheduler: heterogeneous CPUs detected (max frequency: "
					"%" B_PRIu64 ")\n",
					maxFreq);

				// Collect unique capacities
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

				// Sort capacities (bubble sort is fine for few types)
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
						int32 capacity =
							(cpuFreqs[i] * kDefaultCapacity) / maxFreq;
						if (capacity < 128)
							capacity = 128;
						core->SetCapacity(capacity);

						// Map capacity to core type
						for (int32 j = 0; j < uniqueCapacityCount; j++) {
							if (uniqueCapacities[j] == capacity) {
								CoreType type;
								if (uniqueCapacityCount == 1)
									type = CORE_TYPE_STANDARD;
								else if (uniqueCapacityCount == 2)
									type = (j == 0) ? CORE_TYPE_EFFICIENCY
													: CORE_TYPE_PERFORMANCE;
								else {
									// 3 or more types (clamp to max 3)
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

	// Alder Lake fallback heuristic: some hybrid CPUs do not report distinct
	// frequencies for P-cores and E-cores (the firmware reports the P-core
	// boost frequency for all entries). The heuristic assumes the last 8 or
	// 50% of cores (whichever is smaller) are Efficiency cores.
	//
	// IMPORTANT: Only apply this when the frequency API actually confirmed
	// heterogeneity (detectedHeterogeneous == true) but left some cores
	// unclassified (cpu_frequency() returned 0 for them). If the API
	// reported identical frequencies for every core, this is a genuinely
	// homogeneous system and the heuristic must not fire - it would
	// incorrectly split a 16-core Xeon or 64-core EPYC into fake P/E
	// clusters, causing artificial load imbalance and misguided thread
	// coloring with no performance benefit.
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

	// All remaining unclassified cores are STANDARD (covers homogeneous
	// systems of any size and heterogeneous systems where the heuristic
	// left some cores unassigned). We also update the global core type
	// range and detect whether STANDARD cores exist (enabling 3-type
	// intermediate fallbacks in choose_core).
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

	// Calculate dynamic random sampling parameters based on system size
	// We aim for sqrt(N) scaling to maintain coverage without linear cost.
	// Baseline: 16 samples for small/medium systems (up to ~256 packages)
	// Max: 64 samples for massive systems (4096 packages)
	// Simple heuristic: 16 + sqrt(packageCount)
	int32 samples = 16;
	if (packageCount > 16) {
		// Integer square root approximation
		int32 root = 0;
		while ((root + 1) * (root + 1) <= packageCount) root++;
		samples = 16 + root;
	}
	// Clamp to a reasonable maximum to ensure O(1) bound
	if (samples > 64)
		samples = 64;

	gRandomSamples = samples;
	dprintf("scheduler: dynamic random sampling set to %" B_PRId32
			" (packages: %" B_PRId32 ")\n",
			gRandomSamples, packageCount);

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


void scheduler_init() {
	if (get_safemode_boolean("scheduler_topology_tolerant", false))
		sTopologyValidationMode = TOPOLOGY_VALIDATION_TOLERANT;

	int32 cpuCount = smp_get_num_cpus();
	dprintf("scheduler_init: found %" B_PRId32 " logical cpu%s and %" B_PRId32
			" cache level%s\n",
			cpuCount, cpuCount != 1 ? "s" : "", gCPUCacheLevelCount,
			gCPUCacheLevelCount != 1 ? "s" : "");
	dprintf("scheduler_init: topology validation mode: %s\n",
			topology_validation_is_strict() ? "strict" : "tolerant");

#ifdef SCHEDULER_PROFILING
	Profiling::Profiler::Initialize();
#endif

	sThreadDataCache =
		create_object_cache("scheduler thread data", sizeof(ThreadData),
							CACHE_LINE_SIZE, NULL, NULL, NULL);
	if (sThreadDataCache == NULL)
		panic("scheduler_init: failed to create thread data cache");

	status_t result = init();
	if (result != B_OK)
		panic("scheduler_init: failed to initialize scheduler\n");

	scheduler_set_operation_mode(SCHEDULER_MODE_LOW_LATENCY);

	init_debug_commands();

#if SCHEDULER_TRACING
	add_debugger_command_etc(
		"scheduler", &SchedulerTracing::cmd_scheduler,
		"Analyze scheduler tracing information",
		"<thread>\n"
		"Analyzes scheduler tracing information for a given thread.\n"
		"  <thread>  - ID of the thread.\n",
		0);
#endif
}


void scheduler_enable_scheduling() {
	// use atomic store so all CPUs observe the flag immediately.
	StoreRelease(sSchedulerEnabled, 1);
}


void scheduler_update_policy() {
	gTrackCPULoad = increase_cpu_performance(0) == B_OK;
	gTrackCoreLoad = !gSingleCore || gTrackCPULoad;
	dprintf(
		"scheduler switches: single core: %s, cpu load tracking: %s,"
		" core load tracking: %s\n",
		gSingleCore ? "true" : "false", gTrackCPULoad ? "true" : "false",
		gTrackCoreLoad ? "true" : "false");
}

// #pragma mark - SchedulerListener

SchedulerListener::~SchedulerListener() {}

// #pragma mark - kernel private

/*! Add the given scheduler listener. Thread lock must be held.
 */
void scheduler_add_listener(struct SchedulerListener* listener) {
	InterruptsWriteSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Add(listener);
}

/*! Remove the given scheduler listener. Thread lock must be held.
 */
void scheduler_remove_listener(struct SchedulerListener* listener) {
	InterruptsWriteSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Remove(listener);
}

// #pragma mark - Syscalls

bigtime_t _user_estimate_max_scheduling_latency(thread_id id) {
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

	InterruptsLocker _;

	ThreadData* threadData = thread->scheduler_data;
	CoreEntry* core = threadData->Core();
	if (core == NULL || core->Package() == NULL) {
		// Fast-path: Just use the current executing core for the estimate
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


status_t _user_set_scheduler_mode(int32 mode) {
	scheduler_mode schedulerMode = static_cast<scheduler_mode>(mode);
	status_t error = scheduler_set_operation_mode(schedulerMode);
	if (error == B_OK)
		cpu_set_scheduler_mode(schedulerMode);
	return error;
}


int32 _user_get_scheduler_mode() { return Scheduler::Mode(); }

void scheduler_on_team_foreground_changed(Team* team) {
	SCHEDULER_ENTER_FUNCTION();

	// enqueue() acquires CoreCPULocker (fCPULock) and
	// CoreRunQueueLocker (fQueueLock).  If any future code path acquires
	// thread_list_lock while holding fCPULock the original implementation
	// (which called enqueue() inside the thread_list_lock critical section)
	// would deadlock.  To eliminate the risk, collect thread references
	// while holding thread_list_lock, then release the lock before calling
	// the scheduler API.
	//
	// We acquire a BReference to each thread so it cannot be destroyed
	// while we process the list with the lock released.

	// First pass: collect threads under the list lock.
	// Use a fixed-size stack buffer; if the team has more threads than
	// kMaxThreadsPerBatch we process in batches.
	const int kMaxThreadsPerBatch = 256;
	Thread* batch[kMaxThreadsPerBatch];
	bool moreBatches = true;
	Thread* batchStart = NULL;	// NULL = start of list

	// Note: hold an explicit BReference to the cursor thread across batch
	// boundaries, preventing its destruction until we have advanced past it.
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
				// We need a reference for batchStart to survive as the cursor
				// for the next batch.  We already have one from the collection
				// loop above, but we also want to process it in the current
				// batch (which also claims ownership of one reference).
				// So we acquire another one here.
				batchStartRef.SetTo(batchStart, false);
				moreBatches = true;
			}
		}  // thread_list_lock released here

		// Second pass: process collected threads without holding list lock.
		bigtime_t now = system_time();
		for (int i = 0; i < count; i++) {
			Thread* thread = batch[i];
			BReference<Thread> ref(thread, true);

			InterruptsSpinLocker locker(thread->scheduler_lock);
			ThreadData* threadData = thread->scheduler_data;

			if (threadData == NULL || threadData->IsIdle() ||
				threadData->IsRealTime())
				continue;
			if (thread->state == B_THREAD_READY) {
				if (threadData->Dequeue()) {
					threadData->SetForeground(team->fIsForeground);
					threadData->ResetPriorityBoost(now);
					enqueue(thread, false, NULL, now);
				} else {
					threadData->SetForeground(team->fIsForeground);
					threadData->ResetPriorityBoost(now);
				}
			} else {
				threadData->SetForeground(team->fIsForeground);
				threadData->ResetPriorityBoost(now);

				if (thread->state == B_THREAD_RUNNING) {
					ASSERT(thread->cpu != NULL);
					CPUEntry* cpu = &gCPUEntries[thread->cpu->cpu_num];
					if (!gCPU[cpu->ID()].disabled) {
						CoreCPUHeapLocker _(threadData->Core());
						cpu->UpdatePriority(threadData->GetEffectivePriority());
					}
				}
			}

			if (Scheduler::GetCurrentMode()->update_thread_timeslice != NULL) {
				Scheduler::GetCurrentMode()->update_thread_timeslice(
					threadData);
			}
		}
	}
}

}  // namespace Scheduler
