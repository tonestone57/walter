/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */


#include <util/AutoLock.h>

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"


using namespace Scheduler;


const bigtime_t kCacheExpire = 100000;


static void
switch_to_mode()
{
}


static void
set_cpu_enabled(int32 /* cpu */, bool /* enabled */)
{
}


static bool
has_cache_expired(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	if (threadData->WentSleepActive() == 0)
		return false;
	CoreEntry* core = threadData->Core();
	bigtime_t activeTime = core->GetActiveTime();
	return activeTime - threadData->WentSleepActive() > kCacheExpire;
}


static CoreEntry*
choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	// Try to use the previous core if it is idle and we have cache affinity.
	// We also try to use a core on the same package (L3 cache) or a sibling
	// core (L2 cache) to minimize cache misses.
	CoreEntry* previousCore = threadData->PreviousCore();
	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	if (previousCore != NULL && !has_cache_expired(threadData)) {
		if (!useMask || previousCore->CPUMask().Matches(mask)) {
			// Check if previous core is idle
			if (previousCore->GetLoad() == 0)
				return previousCore;

			// Check if previous core is lightly loaded (Soft Affinity)
			// If the load is within the threshold (implying no other core is
			// significantly better, assuming the best alternative is 0 load),
			// we stick to it.
			if (previousCore->GetLoad() <= kLoadDifference)
				return previousCore;
		}

		// Check sibling cores in the same package (likely sharing L3)
		PackageEntry* package = previousCore->Package();
		if (package != NULL) {
			CoreEntry* sibling = package->GetIdleCore();
			if (sibling != NULL && (!useMask || sibling->CPUMask().Matches(mask)))
				return sibling;
		}
	}

	// wake new package
	PackageEntry* package = gIdlePackageList.Last();
	if (package == NULL) {
		// wake new core
		package = PackageEntry::GetMostIdlePackage();
	}

	int32 index = 0;

	CoreEntry* core = NULL;
	if (package != NULL) {
		do {
			core = package->GetIdleCore(index++);
		} while (useMask && core != NULL && !core->CPUMask().Matches(mask));
	}
	if (core == NULL) {
		// no idle cores, use least occupied core
		// iterate over all packages and find the best core
		CoreEntry* bestCore = NULL;
		int32 bestLoad = kMaxLoad + 1;

		for (int32 i = 0; i < gPackageCount; i++) {
			PackageEntry* currentPackage = &gPackageEntries[i];
			currentPackage->ReadLockLoad();

			CoreEntry* candidate = NULL;
			int32 heapIndex = 0;
			// Check LoadHeap
			do {
				candidate = currentPackage->LoadHeap()->PeekMinimum(heapIndex++);
			} while (candidate != NULL && useMask && !candidate->CPUMask().Matches(mask));

			// If not found, check HighLoadHeap
			if (candidate == NULL) {
				heapIndex = 0;
				do {
					candidate = currentPackage->HighLoadHeap()->PeekMinimum(heapIndex++);
				} while (candidate != NULL && useMask && !candidate->CPUMask().Matches(mask));
			}

			if (candidate != NULL) {
				int32 load = candidate->GetLoad();
				if (load < bestLoad) {
					bestLoad = load;
					bestCore = candidate;
				}
			}

			currentPackage->ReadUnlockLoad();
		}
		core = bestCore;
	}

	ASSERT(core != NULL);

	if (previousCore != NULL && !has_cache_expired(threadData)) {
		if (!useMask || previousCore->CPUMask().Matches(mask)) {
			if (core != previousCore) {
				// If the selected core is not significantly less loaded than the
				// previous core, we prefer the previous core to maintain cache locality.
				if (core->GetLoad() + kLoadDifference >= previousCore->GetLoad())
					return previousCore;
			}
		}
	}

	return core;
}


static CoreEntry*
rebalance(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	// Real-time threads bypass rebalancing to ensure zero jitter
	if (threadData->IsRealTime())
		return threadData->Core();

	CoreEntry* core = threadData->Core();
	ASSERT(core != NULL);

	// Get the least loaded core.
	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	CoreEntry* other = NULL;
	int32 bestLoad = kMaxLoad + 1;

	for (int32 i = 0; i < gPackageCount; i++) {
		PackageEntry* currentPackage = &gPackageEntries[i];
		currentPackage->ReadLockLoad();

		CoreEntry* candidate = NULL;
		int32 heapIndex = 0;
		// Check LoadHeap
		do {
			candidate = currentPackage->LoadHeap()->PeekMinimum(heapIndex++);
		} while (candidate != NULL && useMask && !candidate->CPUMask().Matches(mask));

		// If not found, check HighLoadHeap
		if (candidate == NULL) {
			heapIndex = 0;
			do {
				candidate = currentPackage->HighLoadHeap()->PeekMinimum(heapIndex++);
			} while (candidate != NULL && useMask && !candidate->CPUMask().Matches(mask));
		}

		if (candidate != NULL) {
			int32 load = candidate->GetLoad();
			if (load < bestLoad) {
				bestLoad = load;
				other = candidate;
			}
		}

		currentPackage->ReadUnlockLoad();
	}

	ASSERT(other != NULL);

	// Check if the least loaded core is significantly less loaded than
	// the current one.
	int32 coreLoad = core->GetLoad();
	int32 otherLoad = other->GetLoad();

	if (other == core)
		return core;

	bigtime_t coreVRuntime = core->GetMinVirtualRuntime();
	bigtime_t otherVRuntime = other->GetMinVirtualRuntime();

	// If the current core is significantly lagging behind the other core,
	// we lower the threshold for migration to improve latency.
	bool congested = coreVRuntime > 0 && otherVRuntime > coreVRuntime + 20000;
	int32 threshold = congested ? 0 : kLoadDifference;

	if (otherLoad + threshold >= coreLoad)
		return core;

	// Check whether migrating the current thread would result in both core
	// loads become closer to the average.
	int32 difference = coreLoad - otherLoad - threshold;
	ASSERT(difference > 0);

	int32 threadLoad = threadData->GetLoad() / core->CPUCount();
	return difference >= threadLoad ? other : core;
}


static void
rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();

	if (idle)
		return;

	cpu_ent* cpu = get_cpu_struct();
	SpinLocker locker(cpu->irqs_lock);

	irq_assignment* chosen = NULL;
	irq_assignment* irq = (irq_assignment*)list_get_first_item(&cpu->irqs);

	int32 totalLoad = 0;
	while (irq != NULL) {
		if (chosen == NULL || chosen->load < irq->load)
			chosen = irq;
		totalLoad += irq->load;
		irq = (irq_assignment*)list_get_next_item(&cpu->irqs, irq);
	}

	locker.Unlock();

	if (chosen == NULL || totalLoad < kLowLoad)
		return;

	CoreEntry* other = NULL;
	int32 bestLoad = kMaxLoad + 1;

	for (int32 i = 0; i < gPackageCount; i++) {
		PackageEntry* currentPackage = &gPackageEntries[i];
		currentPackage->ReadLockLoad();

		CoreEntry* candidate = currentPackage->LoadHeap()->PeekMinimum();
		if (candidate == NULL)
			candidate = currentPackage->HighLoadHeap()->PeekMinimum();

		if (candidate != NULL) {
			int32 load = candidate->GetLoad();
			if (load < bestLoad) {
				bestLoad = load;
				other = candidate;
			}
		}

		currentPackage->ReadUnlockLoad();
	}

	int32 newCPU = other->CPUHeap()->PeekRoot()->ID();

	ASSERT(other != NULL);

	CoreEntry* core = CoreEntry::GetCore(cpu->cpu_num);
	if (other == core)
		return;
	if (other->GetLoad() + kLoadDifference >= core->GetLoad())
		return;

	assign_io_interrupt_to_cpu(chosen->irq, newCPU);
}


scheduler_mode_operations gSchedulerLowLatencyMode = {
	"low latency",

	1000,
	100,
	{ 2, 5 },

	5000,

	switch_to_mode,
	set_cpu_enabled,
	has_cache_expired,
	choose_core,
	rebalance,
	rebalance_irqs,
};

