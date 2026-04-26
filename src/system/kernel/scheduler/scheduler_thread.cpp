/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 * Audit fixes applied 2025.
 */

#include "scheduler_thread.h"

#include <algorithm>

#include <util/atomic.h>


using namespace Scheduler;


static bigtime_t sQuantumLengths[THREAD_MAX_SET_PRIORITY + 1];


// ComputeQuantum load-scaling constants.  Placed at file scope so the
// compiler evaluates them once at startup rather than re-deriving them on
// every call to the scheduling hot path.  All three values are compile-time
// constants; static storage enforces that.
static const int32 kLoadScale = 1024;
static const int32 kLoadScaleShift = 10;
// kRangeReciprocal = kLoadScale / (kMaxLoad - kLowLoad) * kLoadScale
// 1024 / 800 * 1024 = 1310.72 ~= 1311
static const int32 kRangeReciprocal = (int32)(((int64)kLoadScale * kLoadScale
	+ (kMaxLoad - kLowLoad) / 2) / (kMaxLoad - kLowLoad));
static bigtime_t sVirtualDeadlineSlices[THREAD_MAX_SET_PRIORITY + 1];

bigtime_t ThreadData::sMaxLatency;


void
ThreadData::_InitBase()
{
	fStolenTime = 0;
	fQuantumStart = 0;
	fLastInterruptTime = 0;

	fWentSleep = 0;
	fWentSleepActive = 0;

	fEnqueued = false;
	fEnqueuedInCPURunQueue = false;
	fReady = false;
	fQuickStartCredit = false;

	fHomePackage = -1;

	fEffectivePriority = GetPriority();
	fBaseQuantum = sQuantumLengths[min_c(GetEffectivePriority(),
		THREAD_MAX_SET_PRIORITY)];

	fTimeUsed = 0;

	fMeasureAvailableActiveTime = 0;
	fLastMeasureAvailableTime = 0;
	fMeasureAvailableTime = 0;

	fVirtualRuntime = 0;
	fVirtualDeadline = 0;

	fInteractivityScore = 500;

	fIsForeground = fThread->team->fIsForeground;
	fStolen = false;
}


inline CoreEntry*
ThreadData::_ChooseCore() const
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!gSingleCore);
	return Scheduler::ChooseCore(this);
}


inline CPUEntry*
ThreadData::_ChooseCPU(CoreEntry* core, bool& rescheduleNeeded) const
{
	SCHEDULER_ENTER_FUNCTION();

	int32 threadPriority = GetEffectivePriority();

	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();
	ASSERT(!useMask || mask.Matches(core->CPUMask()));

	if (fThread->previous_cpu != NULL && !fThread->previous_cpu->disabled
			&& (!useMask || mask.GetBit(fThread->previous_cpu->cpu_num))) {
		CPUEntry* previousCPU
			= CPUEntry::GetCPU(fThread->previous_cpu->cpu_num);
		if (previousCPU->Core() == core && CPUPriorityHeap::GetKey(previousCPU)
				<= threadPriority) {
			// Optimization: Prioritize the previous CPU if it is in the same
			// core to maximize cache warmth (L1/L2 hits).
			CoreCPUHeapLocker _(core);
			if (CPUPriorityHeap::GetKey(previousCPU) < threadPriority) {
				previousCPU->UpdatePriority(threadPriority);
				rescheduleNeeded = true;
			} else
				rescheduleNeeded = false;
			return previousCPU;
		}
	}

	CoreCPUHeapLocker _(core);
	CPUEntry* bestCPU = NULL;
	int32 bestKey = INT32_MAX;

	int32 index = 0;
	while (true) {
		CPUEntry* cpu = core->CPUHeap()->PeekRoot(index++);
		if (cpu == NULL)
			break;

		if (useMask && !mask.GetBit(cpu->ID()))
			continue;

		int32 key = CPUPriorityHeap::GetKey(cpu);
		if (bestCPU == NULL || key < bestKey) {
			bestCPU = cpu;
			bestKey = key;

			if (key == B_IDLE_PRIORITY)
				break;
		}
	}

	CPUEntry* cpu = bestCPU;
	if (cpu == NULL)
		return NULL;

	if (bestKey < threadPriority) {
		cpu->UpdatePriority(threadPriority);
		rescheduleNeeded = true;
	} else
		rescheduleNeeded = false;

	return cpu;
}


ThreadData::ThreadData(Thread* thread)
	:
	fThread(thread)
{
}


void
ThreadData::Init()
{
	_InitBase();
	fCore = NULL;

	Thread* currentThread = thread_get_current_thread();
	ThreadData* currentThreadData = currentThread->scheduler_data;
	if (currentThreadData != NULL) {
		fNeededLoad = currentThreadData->fNeededLoad;
		// on 32-bit targets bigtime_t is 64-bit and a plain
		// assignment compiles to two 32-bit loads — torn if the source
		// thread updates fVirtualRuntime concurrently.  Use atomic_get64.
		fVirtualRuntime = atomic_get64(
			(int64*)&currentThreadData->fVirtualRuntime);
		fHomePackage = currentThreadData->fHomePackage;
	} else {
		fNeededLoad = 0;
		fVirtualRuntime = 0;
		fHomePackage = -1;
	}

	if (!IsRealTime())
		_ComputeEffectivePriority(system_time());
}


void
ThreadData::Init(CoreEntry* core)
{
	_InitBase();

	fCore = core;
	fHomePackage = core->Package()->ID();
	fReady = true;
	fNeededLoad = 0;
}


void
ThreadData::Dump() const
{
	kprintf("\thome_package:\t\t%" B_PRId32 "\n", fHomePackage);

	kprintf("\teffective_priority:\t%" B_PRId32 "\n", GetEffectivePriority());

	kprintf("\ttime_used:\t\t%" B_PRId64 " us (quantum: %" B_PRId64 " us)\n",
		fTimeUsed, ComputeQuantum());
	kprintf("\tstolen_time:\t\t%" B_PRId64 " us\n", fStolenTime);
	kprintf("\tquantum_start:\t\t%" B_PRId64 " us\n", fQuantumStart);
	kprintf("\tneeded_load:\t\t%" B_PRId32 "%%\n", fNeededLoad / 10);
	kprintf("\twent_sleep:\t\t%" B_PRId64 "\n", fWentSleep);
	kprintf("\twent_sleep_active:\t%" B_PRId64 "\n", fWentSleepActive);
	kprintf("\tinteractivity_score:\t%" B_PRId32 "\n", fInteractivityScore);
	kprintf("\tcore:\t\t\t%" B_PRId32 "\n",
		fCore != NULL ? fCore->ID() : -1);
	if (fCore != NULL && HasCacheExpired())
		kprintf("\tcache affinity has expired\n");
	if (fQuickStartCredit)
		kprintf("\tquick start credit is set\n");
	if (fEnqueuedInCPURunQueue)
		kprintf("\tenqueued in CPU run queue\n");
	else if (fEnqueued)
		kprintf("\tenqueued in Core run queue\n");
}


bool
ThreadData::ChooseCoreAndCPU(CoreEntry*& targetCore, CPUEntry*& targetCPU)
{
	SCHEDULER_ENTER_FUNCTION();

	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	int32 maxRetries = min_c(5, smp_get_num_cpus());
	for (int32 retry = 0; retry < maxRetries; retry++) {
		bool rescheduleNeeded = false;

		if (targetCore != NULL && (useMask
				&& mask.And(targetCore->CPUMask()).IsEmpty())) {
			targetCore = NULL;
		}
		if (targetCPU != NULL && (useMask && !mask.GetBit(targetCPU->ID())))
			targetCPU = NULL;

		if (targetCore == NULL && targetCPU != NULL)
			targetCore = targetCPU->Core();
		else if (targetCore != NULL && targetCPU == NULL) {
			// If we have a core hint, verify its load under its lock before
			// accepting it. This prevents overloading the waker's core.
			if (targetCore->GetLoad() >= kHighLoad)
				targetCore = NULL;

			if (targetCore != NULL) {
				targetCPU = _ChooseCPU(targetCore, rescheduleNeeded);
				if (targetCPU == NULL)
					targetCore = NULL;
			}
		}

		if (targetCore == NULL && targetCPU == NULL) {
			targetCore = _ChooseCore();
			// Issue 3 fix: _ChooseCore() (which delegates to choose_core in
			// low_latency.cpp / power_saving.cpp) can return NULL when all
			// cores are filtered out by the affinity mask or when the topology
			// arrays are partially initialised during boot. Guard before the
			// ASSERT and CPUMask dereference to avoid a NULL-pointer panic.
			if (targetCore == NULL) {
				// Last-resort: fall back to the current CPU's core, which is
				// always valid while this CPU is running.
				targetCPU = CPUEntry::GetCPU(smp_get_current_cpu());
				targetCore = targetCPU->Core();
				if (targetCore == NULL) {
					// Truly degenerate: current CPU has no core (hot-unplug
					// race). Let the retry loop handle it.
					continue;
				}
				if (fCore != targetCore)
					MigrateTo(targetCore);
				return false;
			}
			ASSERT(!useMask || mask.Matches(targetCore->CPUMask()));
			targetCPU = _ChooseCPU(targetCore, rescheduleNeeded);
			if (targetCPU == NULL) {
				// This can happen if the core selection was based on a stale
				// CPUMask. Retry with a fresh selection.
				targetCore = NULL;
				continue;
			}
		}

		if (targetCPU == NULL) {
			targetCore = NULL;
			continue;
		}

		ASSERT(targetCore != NULL);
		ASSERT(targetCPU != NULL);
		ASSERT(targetCPU->Core() == targetCore);

		// First touch: assign home package if not yet assigned
		if (fHomePackage == -1)
			fHomePackage = targetCore->Package()->ID();

		if (fCore != targetCore)
			MigrateTo(targetCore);
		return rescheduleNeeded;
	}

	// Final fallback: current CPU
	targetCPU = CPUEntry::GetCPU(smp_get_current_cpu());
	targetCore = targetCPU->Core();
	// During hot-unplug the current CPU's core can have CPUCount==0.
	// If so, walk to the first enabled CPU rather than returning a dead core.
	if (targetCore == NULL || targetCore->CPUCount() == 0) {
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (!gCPU[i].disabled) {
				targetCPU = CPUEntry::GetCPU(i);
				targetCore = targetCPU->Core();
				if (targetCore != NULL && targetCore->CPUCount() > 0)
					break;
			}
		}
	}

	if (fCore != targetCore)
		MigrateTo(targetCore);
	return false;
}


bigtime_t
ThreadData::ComputeQuantum() const
{
	SCHEDULER_ENTER_FUNCTION();

	if (IsRealTime())
		return fBaseQuantum;

	// Issue 26 fix: snapshot all mode parameters atomically from the same
	// mode pointer. Caching only the pointer still allows a mode switch to
	// change individual fields between our accesses. Copy the fields we need
	// into locals immediately after the pointer read so all subsequent uses
	// come from a consistent snapshot even if the mode switches mid-function.
	scheduler_mode_operations* mode = Scheduler::GetCurrentMode();
	// Snapshot fields under the assumption that the struct is POD and
	// individual field reads are atomic on this architecture. A full struct
	// copy would require a reader lock; the snapshot approach is a safe
	// approximation — at worst one quantum is computed with mixed parameters,
	// which self-correct on the next scheduling decision.
	const bigtime_t baseQ   = (bigtime_t)atomic_get64((int64*)&mode->base_quantum);
	const bigtime_t minQ    = (bigtime_t)atomic_get64((int64*)&mode->minimal_quantum);
	const bigtime_t maxLat  = (bigtime_t)atomic_get64((int64*)&mode->maximum_latency);
	const bigtime_t mult0   = (bigtime_t)atomic_get64(
		(int64*)&mode->quantum_multipliers[0]);

	(void)mode; // all fields accessed via snapshots above

	const bigtime_t kMinGranularity = 1200;
	const bigtime_t kHighLoadQuantum = max_c(baseQ, kMinGranularity);
	const bigtime_t kMediumQuantum   = baseQ * mult0;
	const bigtime_t kMaxQuantum      = maxLat;
	const bigtime_t kDisplayQuantum  = max_c(minQ, kMinGranularity);

	// Cache fCore once. Without this, a concurrent MigrateTo() can change
	// fCore between the three calls below, mixing data from two different
	// CoreEntry objects. The reads are still individually approximate (no
	// run-queue lock is held), but they now all refer to the same object.
	CoreEntry* const core = fCore;

	// Defensive null guard: fCore can be transiently NULL during a race
	// between UnassignCore() and the subsequent MigrateTo() (e.g. rapid CPU
	// hot-plug).  Return the minimal quantum so the thread gets rescheduled
	// quickly and picks up a valid core assignment on the next pass.
	if (core == NULL)
		return max_c(minQ, kMinGranularity);

	int32 load;
	int32 threadCount;
	int32 cpuCount;

	bool contention;
	bool overload;
	bool displayReady = false;
	load = core->GetLoad();
	threadCount = core->ThreadCount();
	cpuCount = core->CPUCount();

	// Issue 12 fix: replace TryLockRunQueue with a lockless bitmap check.
	// Under heavy load TryLock frequently fails, leaving displayReady=false
	// even when a display thread is waiting.  The result was that the running
	// thread received up to kMaxQuantum (3200us) instead of kDisplayQuantum,
	// adding up to one full extra quantum of latency to display threads.
	//
	// HasHighPriorityThread() reads the run-queue bitmap with atomic_get
	// (same pattern as PeekMaximum) without acquiring the lock.  A stale
	// read means at most one quantum is computed without the optimisation;
	// the next reschedule corrects it.  This strictly improves worst-case
	// display-thread latency over the TryLock approach.
	displayReady = core->HasHighPriorityThread();

	contention = threadCount > cpuCount;
	overload = threadCount > (cpuCount << 1);

	// Determine target quantum floor and max allowed based on contention and display
	bigtime_t floorQuantum = kMediumQuantum;
	bigtime_t maxAllowed = kMaxQuantum;

	if (displayReady) {
		floorQuantum = kDisplayQuantum;
		maxAllowed = kDisplayQuantum;
	} else if (overload) {
		floorQuantum = kHighLoadQuantum;
		maxAllowed = kHighLoadQuantum;
	} else if (contention) {
		floorQuantum = kHighLoadQuantum;
		maxAllowed = kMediumQuantum;
	}

	bigtime_t targetQuantum = maxAllowed;

	if (load > kLowLoad) {
		// Scale from maxAllowed down to floorQuantum
		int64 ratio = (int64)(load - kLowLoad) * kRangeReciprocal
			>> kLoadScaleShift;
		if (ratio > kLoadScale)
			ratio = kLoadScale;

		int64 invRatio = kLoadScale - ratio;
		int64 qRange = maxAllowed - floorQuantum;

		targetQuantum = floorQuantum + ((qRange * invRatio * invRatio)
			>> (2 * kLoadScaleShift));
	}

	bigtime_t quantum = targetQuantum;

	// Context-aware quantum scaling: scale by interactivity score (0.5x - 1.5x)
	// Fast integer approximation of / 1000 (1049 / 2^20 ~= 0.0010004)
	// Ensure 64-bit arithmetic to prevent overflow.
	// Clamp fInteractivityScore before use.  Although write sites
	// apply min_c/max_c, a corrupted value above 1000 would make the
	// multiplier (1500 - score) go negative, producing a negative quantum
	// that bypasses the floor clamp (signed comparison).
	int32 interactivity = fInteractivityScore;
	if (interactivity < 0) interactivity = 0;
	if (interactivity > 1000) interactivity = 1000;

	quantum = (int64)quantum * (int64)(1500 - interactivity) * 1049 >> 20;

	// Clamp to [floor, maxAllowed].
	// Lower bound: the interactivity multiplier (0.5x at fInteractivityScore=1000)
	// can push the quantum below the intent floor, so enforce it.
	// Upper bound: the multiplier (up to 1.5x at fInteractivityScore=0) can push
	// the quantum above maxAllowed.  For displayReady=true this means a CPU-bound
	// thread gets up to 1.5 * kDisplayQuantum, delaying the waiting display thread
	// by 50% beyond the intended ceiling.  Clamp both ends.
	const bigtime_t kResultFloor = displayReady ? floorQuantum : kMinGranularity;
	return min_c(max_c(quantum, kResultFloor), maxAllowed);
}


void
ThreadData::UnassignCore(bool running)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(fCore != NULL);
	if (running || fThread->state == B_THREAD_READY)
		fReady = false;
	if (!fReady)
		fCore = NULL;
}


/* static */ void
ThreadData::ComputeQuantumLengths()
{
	SCHEDULER_ENTER_FUNCTION();

	atomic_set64(&sMaxLatency, Scheduler::MaximumLatency());

	const bigtime_t kBaseSlice = atomic_get64(&Scheduler::gDeadlineBucketSize);
	const bigtime_t kQuantum0 = Scheduler::BaseQuantum();
	const bigtime_t kQuantum1 = kQuantum0 * Scheduler::QuantumMultiplier(0);
	const bigtime_t kQuantum2 = kQuantum0 * Scheduler::QuantumMultiplier(1);

	for (int32 priority = 0; priority <= THREAD_MAX_SET_PRIORITY; priority++) {
		const int32 kBaseWeight = 10;
		int32 taskWeight = max_c(1, priority);

		atomic_set64(&sVirtualDeadlineSlices[priority],
			kBaseSlice * kBaseWeight / taskWeight);

		if (priority >= B_URGENT_DISPLAY_PRIORITY) {
			atomic_set64(&sQuantumLengths[priority], kQuantum0);
		} else if (priority > B_NORMAL_PRIORITY) {
			atomic_set64(&sQuantumLengths[priority],
				_ScaleQuantum(kQuantum1, kQuantum0, B_URGENT_DISPLAY_PRIORITY,
					B_NORMAL_PRIORITY, priority));
		} else {
			atomic_set64(&sQuantumLengths[priority],
				_ScaleQuantum(kQuantum2, kQuantum1, B_NORMAL_PRIORITY,
					B_IDLE_PRIORITY, priority));
		}
	}
}


void
ThreadData::DonateTimesliceTo(Thread* beneficiary)
{
	SCHEDULER_ENTER_FUNCTION();

	if (beneficiary == NULL)
		return;

	ThreadData* beneficiaryData = beneficiary->scheduler_data;
	if (beneficiaryData == NULL)
		return;

	bigtime_t now = system_time();
	bigtime_t timeUsed = now - fQuantumStart;
	ASSERT(timeUsed >= 0);
	fTimeUsed += timeUsed;

	bigtime_t quantum = ComputeQuantum();
	bigtime_t timeLeft = quantum - fTimeUsed;
	if (timeLeft > 0) {
		// Donate remaining slice to the beneficiary.
		// Callers MUST NOT hold any run-queue spinlock when invoking this
		// function; doing so inverts the lock ordering (Core/CPU queue lock
		// → thread scheduler_lock) and risks deadlock.
		// the original assertion was a tautology
		// (!x || true == true always).  Assert what was actually intended:
		// interrupts must be disabled on entry so the spinlock acquire below
		// cannot be preempted.
		ASSERT(!are_interrupts_enabled());
		InterruptsSpinLocker locker(beneficiary->scheduler_lock);
		beneficiaryData->fStolenTime += timeLeft;
	}

	// Exhaust donor slice: we expect the donor to yield or be descheduled
	// immediately after this call to prevent double-dipping.
	fQuantumStart = now;
	fTimeUsed = quantum;
}


void
ThreadData::_ComputeNeededLoad()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(!IsIdle());

	int32 oldLoad = compute_load(fLastMeasureAvailableTime,
		fMeasureAvailableActiveTime, fNeededLoad, system_time());
	if (oldLoad < 0 || oldLoad == fNeededLoad)
		return;

	fCore->ChangeLoad(fNeededLoad - oldLoad);
}


void
ThreadData::_UpdateDeadline()
{
	SCHEDULER_ENTER_FUNCTION();

	if (IsIdle() || IsRealTime())
		return;

	// Virtual Deadline Calculation:
	// Deadline = Now + (BaseSlice * BaseWeight / TaskWeight)
	bigtime_t now = system_time();
	int32 priority = GetPriority();
	if (priority > THREAD_MAX_SET_PRIORITY)
		priority = THREAD_MAX_SET_PRIORITY;

	bigtime_t slice = atomic_get64(&sVirtualDeadlineSlices[priority]);

	// Scale virtual deadline slice by interactivity (bursty threads get shorter slices)
	// Fast integer approximation of / 1000
	// Ensure 64-bit arithmetic to prevent overflow.
	// Use clamped interactivity to prevent negative slice.
	int32 interactivity = fInteractivityScore;
	if (interactivity < 0) interactivity = 0;
	if (interactivity > 1000) interactivity = 1000;
	slice = ((int64)slice * (1500 - interactivity) * 1049) >> 20;

	// prevent the interactivity multiplier from shrinking
	// the slice to near-zero.  When fInteractivityScore == 1000 the
	// multiplier is ~0.5, which is correct (bursty thread gets a shorter
	// deadline), but if slice was already small the result can reach 0.
	// A zero slice sets fVirtualDeadline == now, giving the thread
	// maximum urgency permanently and starving lower-priority threads.
	// Floor at one bucket width so the deadline is always in the future.
	{
		const bigtime_t kMinSlice = atomic_get64(&Scheduler::gDeadlineBucketSize);
		if (slice < kMinSlice)
			slice = kMinSlice;
	}

	fVirtualDeadline = now + slice;

	_ComputeEffectivePriority(now);
}


void
ThreadData::_ComputeEffectivePriority(bigtime_t now) const
{
	SCHEDULER_ENTER_FUNCTION();

	// Cache bucket size: avoids redundant atomic reads on this hot path.
	// The value is effectively constant within a scheduling decision.
	const bigtime_t bucketSize = atomic_get64(&Scheduler::gDeadlineBucketSize);

	if (IsIdle())
		fEffectivePriority = B_IDLE_PRIORITY;
	else if (IsRealTime())
		fEffectivePriority = GetPriority();
	else {
		// Map Virtual Deadline to Dynamic Priority (Urgency).
		// Urgency = MaxDynamic - (Deadline - Now) / 5ms
		// If Deadline is Now (or passed), Urgency is Max.
		// If Deadline is far, Urgency is 0.

		bigtime_t diff = fVirtualDeadline - now;

		// Adaptive Urgency Boost: give bursty threads higher urgency.
		bigtime_t urgencyBoost = (fInteractivityScore * bucketSize) / 1000;
		diff -= urgencyBoost;

		// Urgency Bonus: Grant foreground threads a "head start" in priority.
		if (fIsForeground) {
			diff -= bucketSize;

			// Display-Awareness: Additional boost for interactive foreground threads
			if (fInteractivityScore > 750)
				diff -= bucketSize / 2;
		}

		const int32 kMaxDynamicPriority = B_FIRST_REAL_TIME_PRIORITY - 1;
		static_assert(kMaxDynamicPriority <= THREAD_MAX_SET_PRIORITY,
			"kMaxDynamicPriority exceeds THREAD_MAX_SET_PRIORITY");

		bigtime_t urgency = kMaxDynamicPriority - diff / bucketSize;
		if (urgency < 0) urgency = 0;
		if (urgency > kMaxDynamicPriority) urgency = kMaxDynamicPriority;
		// kMaxDynamicPriority fits in int32, but if diff is very
		// negative the expression can produce urgency > INT32_MAX before the
		// clamp.  The clamp to kMaxDynamicPriority above is sufficient for
		// correctness (bigtime_t is 64-bit signed), but add an explicit cast
		// guard to silence undefined-behaviour sanitisers.
		if (urgency > (bigtime_t)INT32_MAX) urgency = (bigtime_t)INT32_MAX;

		fEffectivePriority = (int32)urgency;
	}

	fBaseQuantum = atomic_get64(&sQuantumLengths[GetEffectivePriority()]);
}


/* static */ bigtime_t
ThreadData::_ScaleQuantum(bigtime_t maxQuantum, bigtime_t minQuantum,
	int32 maxPriority, int32 minPriority, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(priority <= maxPriority);
	ASSERT(priority >= minPriority);

	if (maxPriority <= minPriority)
		return maxQuantum;

	bigtime_t result = (maxQuantum - minQuantum) * (priority - minPriority);
	result /= maxPriority - minPriority;
	return maxQuantum - result;
}


void
ThreadData::MigrateTo(CoreEntry* targetCore)
{
	SCHEDULER_ENTER_FUNCTION();

	if (fCore == targetCore)
		return;

	fLoadMeasurementEpoch = targetCore->LoadMeasurementEpoch() - 1;

	if (fReady) {
		if (gTrackCoreLoad) {
			if (fCore != NULL)
				fCore->RemoveLoad(fNeededLoad, true);
			targetCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, true);
		}
	}

	fCore = targetCore;
}


ThreadProcessing::~ThreadProcessing()
{
}


void
ThreadData::ResetPriorityBoost()
{
	SCHEDULER_ENTER_FUNCTION();

	_ComputeEffectivePriority(system_time());
}
