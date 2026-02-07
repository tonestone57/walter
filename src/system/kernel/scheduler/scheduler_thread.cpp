/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_thread.h"

#include <algorithm>

#include <util/atomic.h>


using namespace Scheduler;


static const bigtime_t kDeadlineBucketSize = 5000;
static bigtime_t sQuantumLengths[THREAD_MAX_SET_PRIORITY + 1];
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
}


inline CoreEntry*
ThreadData::_ChooseCore() const
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!gSingleCore);
	return gCurrentMode->choose_core(this);
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
		if (previousCPU->Core() == core) {
			CoreCPUHeapLocker _(core);
			if (CPUPriorityHeap::GetKey(previousCPU) < threadPriority) {
				previousCPU->UpdatePriority(threadPriority);
				rescheduleNeeded = true;
				return previousCPU;
			}
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
	if (cpu == NULL) {
		// Should not happen if the core mask intersection is valid
		// Fallback to the first CPU in the core if possible, or panic?
		// ChooseCoreAndCPU logic implies valid intersection.
		// If useMask is true, we must have found something.
		// If useMask is false, we definitely found something (root).
		panic("scheduler: no valid CPU found in core %" B_PRId32, core->ID());
	}

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
	fNeededLoad = currentThreadData->fNeededLoad;
	fVirtualRuntime = currentThreadData->fVirtualRuntime;
	fHomePackage = currentThreadData->fHomePackage;

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

	bool rescheduleNeeded = false;

	if (targetCore != NULL && targetCPU != NULL) {
		if (targetCPU->Core() != targetCore)
			targetCore = targetCPU->Core();
	}

	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	if (targetCore != NULL && (useMask
			&& GetCPUMask().And(targetCore->CPUMask()).IsEmpty())) {
		targetCore = NULL;
	}
	if (targetCPU != NULL && (useMask && !mask.GetBit(targetCPU->ID())))
		targetCPU = NULL;

	if (targetCore == NULL && targetCPU != NULL)
		targetCore = targetCPU->Core();
	else if (targetCore != NULL && targetCPU == NULL)
		targetCPU = _ChooseCPU(targetCore, rescheduleNeeded);
	else if (targetCore == NULL && targetCPU == NULL) {
		targetCore = _ChooseCore();
		ASSERT(!useMask || mask.Matches(targetCore->CPUMask()));
		targetCPU = _ChooseCPU(targetCore, rescheduleNeeded);
	}

	ASSERT(targetCore != NULL);
	ASSERT(targetCPU != NULL);

	// First touch: assign home package if not yet assigned
	if (fHomePackage == -1)
		fHomePackage = targetCore->Package()->ID();

	if (fCore != targetCore) {
		fLoadMeasurementEpoch = targetCore->LoadMeasurementEpoch() - 1;
		if (fReady) {
			if (fCore != NULL)
				fCore->RemoveLoad(fNeededLoad, true);
			targetCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, true);
		}
	}

	fCore = targetCore;
	return rescheduleNeeded;
}


bigtime_t
ThreadData::ComputeQuantum() const
{
	SCHEDULER_ENTER_FUNCTION();

	if (IsRealTime())
		return fBaseQuantum;

	const bigtime_t kMinGranularity = 1200;
	const bigtime_t kHighLoadQuantum = std::max(gCurrentMode->base_quantum,
		kMinGranularity);
	const bigtime_t kMediumQuantum = gCurrentMode->base_quantum
		* gCurrentMode->quantum_multipliers[0];
	const bigtime_t kMaxQuantum = gCurrentMode->maximum_latency;
	const bigtime_t kDisplayQuantum = std::max(gCurrentMode->minimal_quantum,
		kMinGranularity);

	const int32 kLoadScale = 1024;
	const int32 kLoadScaleShift = 10;
	// kRangeReciprocal = kLoadScale / (kMaxLoad - kLowLoad) * kLoadScale
	// 1024 / 800 * 1024 = 1310.72 ~= 1311
	const int32 kRangeReciprocal = (int32)(((int64)kLoadScale * kLoadScale
		+ (kMaxLoad - kLowLoad) / 2) / (kMaxLoad - kLowLoad));

	int32 load = fCore->GetLoad();
	int32 threadCount = fCore->ThreadCount();
	int32 cpuCount = fCore->CPUCount();

	bool contention = threadCount > cpuCount;
	bool overload = threadCount > (cpuCount << 1);
	bool displayReady = false;

	ThreadData* next = fCore->PeekHead();
	if (next != NULL && next->GetEffectivePriority() >= B_DISPLAY_PRIORITY)
		displayReady = true;

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

	return std::max(quantum, kMinGranularity);
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

	for (int32 priority = 0; priority <= THREAD_MAX_SET_PRIORITY; priority++) {
		const bigtime_t kBaseSlice = kDeadlineBucketSize;
		const int32 kBaseWeight = 10;
		int32 taskWeight = max_c(1, priority);

		atomic_set64(&sVirtualDeadlineSlices[priority],
			kBaseSlice * kBaseWeight / taskWeight);
	}

	for (int32 priority = 0; priority <= THREAD_MAX_SET_PRIORITY; priority++) {
		const bigtime_t kQuantum0 = gCurrentMode->base_quantum;
		if (priority >= B_URGENT_DISPLAY_PRIORITY) {
			atomic_set64(&sQuantumLengths[priority], kQuantum0);
			continue;
		}

		const bigtime_t kQuantum1
			= kQuantum0 * gCurrentMode->quantum_multipliers[0];
		if (priority > B_NORMAL_PRIORITY) {
			atomic_set64(&sQuantumLengths[priority],
				_ScaleQuantum(kQuantum1, kQuantum0, B_URGENT_DISPLAY_PRIORITY,
					B_NORMAL_PRIORITY, priority));
			continue;
		}

		const bigtime_t kQuantum2
			= kQuantum0 * gCurrentMode->quantum_multipliers[1];
		atomic_set64(&sQuantumLengths[priority],
			_ScaleQuantum(kQuantum2, kQuantum1, B_NORMAL_PRIORITY,
				B_IDLE_PRIORITY, priority));
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

	fTimeUsed = 0;
}


void
ThreadData::_ComputeNeededLoad()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(!IsIdle());

	int32 oldLoad = compute_load(fLastMeasureAvailableTime,
		fMeasureAvailableActiveTime, fNeededLoad, fMeasureAvailableTime);
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

	fVirtualDeadline = now + atomic_get64(&sVirtualDeadlineSlices[priority]);

	_ComputeEffectivePriority(now);
}


void
ThreadData::_ComputeEffectivePriority(bigtime_t now) const
{
	SCHEDULER_ENTER_FUNCTION();

	if (IsIdle())
		fEffectivePriority = B_IDLE_PRIORITY;
	else if (IsRealTime())
		fEffectivePriority = GetPriority();
	else {
		// Map Virtual Deadline to Dynamic Priority (Urgency).
		// Urgency = MaxDynamic - (Deadline - Now) / 5ms
		// If Deadline is Now (or passed), Urgency is Max.
		// If Deadline is far, Urgency is 0.

		const int32 kMaxDynamicPriority = B_FIRST_REAL_TIME_PRIORITY - 1;
		bigtime_t urgency = kMaxDynamicPriority
			- (fVirtualDeadline - now) / kDeadlineBucketSize;
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

	if (fReady) {
		if (gTrackCoreLoad) {
			if (fCore != NULL)
				fCore->RemoveLoad(fNeededLoad, true);
			targetCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, true);
		}
	}

	fLoadMeasurementEpoch = targetCore->LoadMeasurementEpoch() - 1;
	fCore = targetCore;
}


ThreadProcessing::~ThreadProcessing()
{
}

