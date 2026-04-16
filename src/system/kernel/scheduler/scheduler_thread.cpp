/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
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
		fVirtualRuntime = currentThreadData->fVirtualRuntime;
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
				&& GetCPUMask().And(targetCore->CPUMask()).IsEmpty())) {
			targetCore = NULL;
		}
		if (targetCPU != NULL && (useMask && !mask.GetBit(targetCPU->ID())))
			targetCPU = NULL;

		if (targetCore == NULL && targetCPU != NULL)
			targetCore = targetCPU->Core();
		else if (targetCore != NULL && targetCPU == NULL) {
			targetCPU = _ChooseCPU(targetCore, rescheduleNeeded);
			if (targetCPU == NULL)
				targetCore = NULL;
		}

		if (targetCore == NULL && targetCPU == NULL) {
			targetCore = _ChooseCore();
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

	const bigtime_t kMinGranularity = 1200;
	const bigtime_t kHighLoadQuantum = max_c(Scheduler::BaseQuantum(),
		kMinGranularity);
	const bigtime_t kMediumQuantum = Scheduler::BaseQuantum()
		* Scheduler::QuantumMultiplier(0);
	const bigtime_t kMaxQuantum = Scheduler::MaximumLatency();
	const bigtime_t kDisplayQuantum = max_c(Scheduler::MinimalQuantum(),
		kMinGranularity);

	// Cache fCore once. Without this, a concurrent MigrateTo() can change
	// fCore between the three calls below, mixing data from two different
	// CoreEntry objects. The reads are still individually approximate (no
	// run-queue lock is held), but they now all refer to the same object.
	CoreEntry* const core = fCore;

	// Defensive null guard: fCore can be transiently NULL during a race
	// between UnassignCore() and the subsequent MigrateTo() (e.g. rapid CPU
	// hot-plug).  Return the minimal quantum so the thread gets rescheduled
	// quickly and picks up a valid core assignment on the next pass.
	if (core == NULL) {
		bigtime_t minQ = Scheduler::MinimalQuantum();
		return max_c(minQ, kMinGranularity);
	}

	int32 load;
	int32 threadCount;
	int32 cpuCount;

	bool contention;
	bool overload;
	bool displayReady = false;
	{
		CoreRunQueueLocker _(core);
		load = core->GetLoad();
		threadCount = core->ThreadCount();
		cpuCount = core->CPUCount();

		ThreadData* next = core->PeekHead();
		if (next != NULL && next->GetEffectivePriority() >= B_DISPLAY_PRIORITY)
			displayReady = true;
	}

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
	quantum = ((int64)quantum * (1500 - fInteractivityScore) * 1049) >> 20;

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

	bigtime_t timeUsed = system_time() - fQuantumStart;
	ASSERT(timeUsed >= 0);
	fTimeUsed += timeUsed;

	bigtime_t timeLeft = ComputeQuantum() - fTimeUsed;
	if (timeLeft > 0) {
		InterruptsSpinLocker locker(beneficiary->scheduler_lock);
		beneficiaryData->fStolenTime += timeLeft;
	}

	// Exhaust donor slice: we expect the donor to yield or be descheduled
	// immediately after this call to prevent double-dipping.
	fQuantumStart = system_time();
	fTimeUsed = ComputeQuantum();
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
	slice = ((int64)slice * (1500 - fInteractivityScore) * 1049) >> 20;

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
		bigtime_t urgency = kMaxDynamicPriority - diff / bucketSize;
		if (urgency < 0) urgency = 0;
		if (urgency > kMaxDynamicPriority) urgency = kMaxDynamicPriority;

		fEffectivePriority = (int32)urgency;
	}

	int32 effectivePriority = GetEffectivePriority();
	if (effectivePriority > THREAD_MAX_SET_PRIORITY)
		effectivePriority = THREAD_MAX_SET_PRIORITY;

	fBaseQuantum = atomic_get64(&sQuantumLengths[effectivePriority]);
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
