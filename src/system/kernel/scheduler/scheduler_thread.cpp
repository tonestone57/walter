// AUDIT FIXES: issues 11, 14, 33, 37, 56, 64, 72, 76, 77
/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 * Audit fixes applied 2025.
 */

#include "scheduler_thread.h"

#include <algorithm>

#include <util/atomic.h>


using namespace Scheduler;


static bigtime_t sQuantumLengths[THREAD_MAX_SET_PRIORITY + 1]
	__attribute__((aligned(8)));


static const int32 kLoadScale = 1024;
static const int32 kLoadScaleShift = 10;
static const int32 kRangeReciprocal = (int32)(((int64)kLoadScale * kLoadScale
	+ (kMaxLoad - kLowLoad) / 2) / (kMaxLoad - kLowLoad));
static bigtime_t sVirtualDeadlineSlices[THREAD_MAX_SET_PRIORITY + 1]
	__attribute__((aligned(8)));

bigtime_t ThreadData::sMaxLatency __attribute__((aligned(8)));


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
		// Issue 33 fix: use a sequence-count retry loop to get a consistent
		// snapshot of fHomePackage and fVirtualRuntime from a concurrent
		// source thread.
		int32 homeA, homeB;
		bigtime_t vrt;
		int retries = 0;
		do {
			homeA = atomic_get(const_cast<int32*>(&currentThreadData->fHomePackage));
			memory_read_barrier();
			vrt = atomic_get64((int64*)&currentThreadData->fVirtualRuntime);
			memory_read_barrier();
			homeB = atomic_get(const_cast<int32*>(&currentThreadData->fHomePackage));
		} while (homeA != homeB && ++retries < 8);
		fVirtualRuntime = vrt;
		fHomePackage = homeB;
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
			if (targetCore == NULL) {
				targetCPU = CPUEntry::GetCPU(smp_get_current_cpu());
				targetCore = targetCPU->Core();
				if (targetCore == NULL) {
					continue;
				}
				if (fCore != targetCore)
					MigrateTo(targetCore);
				return false;
			}
			ASSERT(!useMask || mask.Matches(targetCore->CPUMask()));
			targetCPU = _ChooseCPU(targetCore, rescheduleNeeded);
			if (targetCPU == NULL) {
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

		if (fHomePackage == -1)
			fHomePackage = targetCore->Package()->ID();

		if (fCore != targetCore)
			MigrateTo(targetCore);
		return rescheduleNeeded;
	}

	targetCPU = CPUEntry::GetCPU(smp_get_current_cpu());
	targetCore = targetCPU->Core();
	if (targetCore == NULL || targetCore->CPUCount() == 0) {
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (!gCPU[i].disabled) {
				if (!mask.IsEmpty() && !mask.GetBit(i))
					continue;
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

	scheduler_mode_operations* mode = Scheduler::GetCurrentMode();
	const bigtime_t baseQ  = mode->base_quantum;
	const bigtime_t minQ   = mode->minimal_quantum;
	const bigtime_t maxLat = mode->maximum_latency;
	const bigtime_t mult0  = mode->quantum_multipliers[0];

	const bigtime_t kMinGranularity = 1200;

	CoreEntry* const core = fCore;

	if (core == NULL)
		return max_c(minQ, kMinGranularity);

	// Issue 76 fix: guard against zero capacity or score factor to prevent
	// division-by-zero panics during early boot or mode switches.
	if (core->Capacity() <= 0 || core->ScoreFactor() == 0)
		return max_c(minQ, kMinGranularity);

	const bigtime_t kHighLoadQuantum = max_c(baseQ, kMinGranularity);
	const bigtime_t kMediumQuantum   = baseQ * mult0;
	const bigtime_t kMaxQuantum      = maxLat;
	const bigtime_t kDisplayQuantum  = max_c(minQ, kMinGranularity);

	int32 load;
	int32 threadCount;
	int32 cpuCount;

	bool contention;
	bool overload;
	bool displayReady = false;
	load = core->GetLoad();
	threadCount = core->ThreadCount();
	cpuCount = core->CPUCount();

	displayReady = core->HasHighPriorityThread();

	contention = threadCount > cpuCount;
	overload = threadCount > (cpuCount << 1);

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

	int32 interactivity = fInteractivityScore;
	if (interactivity < 0) interactivity = 0;
	if (interactivity > 1000) interactivity = 1000;

	quantum = (int64)quantum * (int64)(1500 - interactivity) * 1049 >> 20;

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

	const bigtime_t kBaseSlice = atomic_get64(
		const_cast<int64*>(&Scheduler::gDeadlineBucketSize));
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
		ASSERT(!are_interrupts_enabled());
		SpinLocker locker(beneficiary->scheduler_lock);
		beneficiaryData->fStolenTime += timeLeft;
	}

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
	if (oldLoad < 0)
		return;
	if (oldLoad == fNeededLoad)
		return;

	fCore->ChangeLoad(fNeededLoad - oldLoad);
}


void
ThreadData::_UpdateDeadline()
{
	SCHEDULER_ENTER_FUNCTION();

	if (IsIdle() || IsRealTime())
		return;

	bigtime_t now = system_time();
	int32 priority = GetPriority();
	if (priority > THREAD_MAX_SET_PRIORITY)
		priority = THREAD_MAX_SET_PRIORITY;

	bigtime_t slice = atomic_get64(&sVirtualDeadlineSlices[priority]);

	int32 interactivity = fInteractivityScore;
	if (interactivity < 0) interactivity = 0;
	if (interactivity > 1000) interactivity = 1000;
	slice = ((int64)slice * (1500 - interactivity) * 1049) >> 20;

	{
		// Issue 37 fix: floor the virtual deadline slice at gDeadlineBucketSize/4
		// to prevent interactivity scoring from causing starvation.
		const bigtime_t kMinSlice = Scheduler::GetCurrentMode()->base_quantum / 4;
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

	const bigtime_t bucketSize = atomic_get64(
		const_cast<int64*>(&Scheduler::gDeadlineBucketSize));

	if (bucketSize <= 0) {
		fEffectivePriority = GetPriority();
		fBaseQuantum = Scheduler::MinimalQuantum();
		return;
	}

	if (IsIdle())
		fEffectivePriority = B_IDLE_PRIORITY;
	else if (IsRealTime())
		fEffectivePriority = GetPriority();
	else {
		bigtime_t diff = fVirtualDeadline - now;

		bigtime_t urgencyBoost = (fInteractivityScore * bucketSize) / 1000;

		if (urgencyBoost > 0) {
			// Issue 56 fix: clamp urgency boost to B_INT64_MIN to prevent
			// signed underflow and subsequent priority inversion.
			if (diff > B_INT64_MIN + (bigtime_t)urgencyBoost)
				diff -= urgencyBoost;
			else
				diff = B_INT64_MIN;
		}

		if (fIsForeground) {
			if (bucketSize > 0 && diff > B_INT64_MIN + bucketSize)
				diff -= bucketSize;
			else if (bucketSize > 0)
				diff = B_INT64_MIN;

			if (fInteractivityScore > 750)
			{
				bigtime_t half = bucketSize / 2;
				if (half > 0 && diff > B_INT64_MIN + half)
					diff -= half;
				else if (half > 0)
					diff = B_INT64_MIN;
			}
		}

		const int32 kMaxDynamicPriority = B_FIRST_REAL_TIME_PRIORITY - 1;

		bigtime_t urgency = kMaxDynamicPriority - diff / bucketSize;
		if (urgency < 0) urgency = 0;
		if (urgency > kMaxDynamicPriority) urgency = kMaxDynamicPriority;
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

	// Issue 77 fix: use intentional unsigned underflow (epoch - 1) to force
	// load updates on the target core.
	uint32 targetEpoch = targetCore->LoadMeasurementEpoch();
	fLoadMeasurementEpoch = targetEpoch - 1;

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

	if (!IsIdle() && !IsRealTime())
		_UpdateDeadline();

	_ComputeEffectivePriority(system_time());
}
