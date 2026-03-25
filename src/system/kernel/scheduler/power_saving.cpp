/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */


#include <util/atomic.h>
#include <util/AutoLock.h>
#include <util/Random.h>

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"
#include "scheduler_topology.h"


using namespace Scheduler;


static CoreEntry* sSmallTaskCore;


static void
switch_to_mode()
{
	atomic_pointer_set(&sSmallTaskCore, (CoreEntry*)NULL);
}


static void
set_cpu_enabled(int32 cpu, bool enabled)
{
	if (!enabled)
		atomic_pointer_set(&sSmallTaskCore, (CoreEntry*)NULL);
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


static void
check_package_small_task(PackageEntry* entry, CoreEntry*& core, int32& bestLoad)
{
	// Find the core with highest load that isn't overloaded.
	// If all are overloaded, we might pick the least loaded later.
	// For choosing a small task core, we want packing.
	CoreEntry* candidate = entry->PeekMaximumLoadCore();

	if (candidate != NULL) {
		if (candidate->GetLoad() >= kHighLoad) {
			// The busiest is overloaded. Check if there is a less loaded one.
			candidate = entry->PeekMinimumLoadCore();
		}
	}

	if (candidate != NULL) {
		int32 load = candidate->GetLoad();
		if (core == NULL) {
			core = candidate;
			bestLoad = load;
		} else {
			bool bestOverloaded = bestLoad >= kHighLoad;
			bool candidateOverloaded = load >= kHighLoad;

			if (candidateOverloaded) {
				// If candidate is overloaded, we only pick it if current is ALSO overloaded
				// AND candidate is LESS loaded (minimize overload).
				if (bestOverloaded && load < bestLoad) {
					core = candidate;
					bestLoad = load;
				}
			} else {
				// Candidate is NOT overloaded.
				// If current is overloaded, we definitely switch.
				// If current is NOT overloaded, we switch if candidate is BUSIER (packing).
				if (bestOverloaded || load > bestLoad) {
					core = candidate;
					bestLoad = load;
				}
			}
		}
	}
}


static CoreEntry*
choose_small_task_core()
{
	SCHEDULER_ENTER_FUNCTION();

	CoreEntry* core = NULL;
	int32 bestLoad = -1;

	bool tryRandom = gPackageCount > kRandomSearchThreshold;
	if (tryRandom) {
		search_global_random([&](PackageEntry* entry) {
			check_package_small_task(entry, core, bestLoad);
			return false;
		});
	}

	// Fallback to full scan if random sampling failed to find a candidate
	// or if system is small.
	if (core == NULL) {
		const int32 kMaxFallbackAttempts = 64;
		int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);
		int32 startIndex = tryRandom ? CPUEntry::GetCPU(smp_get_current_cpu())->GetRandom() % gPackageCount : 0;

		for (int32 i = 0; i < attempts; i++) {
			int32 index = startIndex + i;
			if (index >= gPackageCount)
				index -= gPackageCount;
			check_package_small_task(&gPackageEntries[index], core, bestLoad);
		}
	}

	if (core == NULL)
		return (CoreEntry*)atomic_pointer_get(&sSmallTaskCore);

	CoreEntry* smallTaskCore
		= (CoreEntry*)atomic_pointer_test_and_set(&sSmallTaskCore, core,
			(CoreEntry*)NULL);
	if (smallTaskCore == NULL)
		return core;
	return smallTaskCore;
}


static CoreEntry*
choose_idle_core()
{
	SCHEDULER_ENTER_FUNCTION();

	PackageEntry* package = PackageEntry::GetLeastIdlePackage();

	if (package == NULL) {
		// No partially idle packages. Check for any idle package using the mask.
		uint64 idleNodeMask = atomic_get64((int64*)&gIdleNodeMask);
		while (idleNodeMask != 0) {
			int32 nodeIndex = __builtin_ctzll(idleNodeMask);
			idleNodeMask &= ~(1ULL << nodeIndex);

			SchedulerNode* node = &gSchedulerNodes[nodeIndex];
			uint64 idlePackageMask = node->IdlePackageMask();

			if (idlePackageMask != 0) {
				int32 packageIndex = __builtin_ctzll(idlePackageMask);
				// fPackageStartIndex + packageIndex gives global index
				int32 globalIndex = node->PackageStartIndex() + packageIndex;
				package = &gPackageEntries[globalIndex];
				break;
			}
		}
	}

	if (package != NULL)
		return package->GetIdleCore();
	return NULL;
}




static void
check_package_packing(PackageEntry* entry, const CPUSet* mask,
	CoreEntry*& other, int32& bestLoad, bool& foundNonOverloaded)
{
	// We want to pack: find the busiest core that is NOT overloaded (load < kHighLoad).
	// If all active cores are overloaded, pick the least loaded one (to minimize overload).
	CoreEntry* candidate = entry->PeekMaximumLoadCore(mask);

	if (candidate != NULL) {
		if (candidate->GetLoad() >= kHighLoad) {
			// The busiest is overloaded. Check if there is a less loaded one.
			candidate = entry->PeekMinimumLoadCore(mask);
		}
	}

	if (candidate != NULL) {
		int32 load = candidate->GetLoad();
		bool isOverloaded = load >= kHighLoad;

		if (other == NULL) {
			other = candidate;
			bestLoad = load;
			foundNonOverloaded = !isOverloaded;
		} else if (foundNonOverloaded) {
			if (!isOverloaded) {
				// Both are non-overloaded. Pick the BUSIEST (packing).
				if (load > bestLoad) {
					other = candidate;
					bestLoad = load;
				}
			}
			// If candidate is overloaded, ignore it (we prefer the existing non-overloaded 'other')
		} else {
			if (!isOverloaded) {
				// Found a non-overloaded core! It beats the current overloaded 'other'.
				other = candidate;
				bestLoad = load;
				foundNonOverloaded = true;
			} else {
				// Both are overloaded. Pick the LEAST loaded (spread overload).
				if (load < bestLoad) {
					other = candidate;
					bestLoad = load;
				}
			}
		}
	}
}


static void
check_masked_packages_packing(const CPUSet& mask, CoreEntry*& other,
	int32& bestLoad, bool& foundNonOverloaded)
{
	const int32 kCPUSetArraySize = (SMP_MAX_CPUS + 31) / 32;
	const int32 cpuCount = smp_get_num_cpus();
	PackageEntry* lastPackage = NULL;

	for (int32 i = 0; i < kCPUSetArraySize; i++) {
		uint32 bits = mask.Bits(i);
		if (bits == 0)
			continue;

		while (bits != 0) {
			int bit = __builtin_ctz(bits);
			bits &= ~(1U << bit);
			int32 cpuID = i * 32 + bit;

			if (cpuID >= cpuCount)
				continue;

			CoreEntry* cpuCore = CPUEntry::GetCPU(cpuID)->Core();
			if (cpuCore != NULL) {
				PackageEntry* package = cpuCore->Package();
				if (package != NULL && package != lastPackage) {
					check_package_packing(package, &mask, other, bestLoad, foundNonOverloaded);
					lastPackage = package;
				}
			}
		}
	}
}




static CoreEntry*
choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	CoreEntry* core = NULL;

	CPUSet mask = threadData->GetCPUMask();
	bool useMask = !mask.IsEmpty();

	// Optimization: If the mask is effectively "all enabled CPUs", treat it as no mask
	if (useMask) {
		const int32 kCPUSetArraySize = (SMP_MAX_CPUS + 31) / 32;
		bool allEnabled = true;
		for (int32 i = 0; i < kCPUSetArraySize; i++) {
			if (mask.Bits(i) != gCPUEnabled.Bits(i)) {
				allEnabled = false;
				break;
			}
		}
		if (allEnabled)
			useMask = false;
	}

	// try to pack all threads on one core
	core = choose_small_task_core();
	if (core != NULL && (useMask && !core->CPUMask().Matches(mask)))
		core = NULL;

	if (core == NULL || core->GetLoad() + threadData->GetLoad() >= kHighLoad) {
		// run immediately on already woken core
		CoreEntry* bestCore = NULL;
		int32 bestLoad = -1;

		bool tryRandom = gPackageCount > kRandomSearchThreshold;

		if (tryRandom && !useMask) {
			CoreEntry* previousCore = threadData->PreviousCore();

			// Phase 1: L3 Domain (Sibling in previous package)
			if (previousCore != NULL && !has_cache_expired(threadData)) {
				PackageEntry* package = previousCore->Package();
				if (package != NULL) {
					CheckPackageMinimumLoad(package, NULL, bestCore, bestLoad);
				}
			}

			// Phase 2: Local Node
			SchedulerNode* node = NULL;
			if (previousCore != NULL)
				node = previousCore->Package()->Node();
			else if (threadData->HomePackage() >= 0
				&& threadData->HomePackage() < gPackageCount) {
				node = gPackageEntries[threadData->HomePackage()].Node();
			}

			search_local_node(node, [&](PackageEntry* entry) {
				CheckPackageMinimumLoad(entry, NULL, bestCore, bestLoad);
				return false;
			});

			// Phase 3: Global Random
			search_global_random([&](PackageEntry* entry) {
				CheckPackageMinimumLoad(entry, NULL, bestCore, bestLoad);
				return false;
			});

		} else if (useMask) {
			CheckMaskedPackagesMinimumLoad(mask, bestCore, bestLoad);
		}

		// Fallback to full scan
		if (bestCore == NULL && !useMask) {
			const int32 kMaxFallbackAttempts = 64;
			int32 startIndex = tryRandom ? CPUEntry::GetCPU(smp_get_current_cpu())->GetRandom() % gPackageCount : 0;
			int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

			for (int32 i = 0; i < attempts; i++) {
				int32 index = startIndex + i;
				if (index >= gPackageCount)
					index -= gPackageCount;
				CheckPackageMinimumLoad(&gPackageEntries[index], NULL, bestCore,
					bestLoad);
			}
		}

		core = bestCore;

		if (core == NULL) {
			core = choose_idle_core();
			if (core != NULL && useMask && !core->CPUMask().Matches(mask))
				core = NULL;
		}
	}

	if (core == NULL) {
		core = CoreEntry::GetCore(smp_get_current_cpu());
		if (useMask && !core->CPUMask().Matches(mask)) {
			// fallback to the first valid core
			core = NULL;
			const int32 kCPUSetArraySize = (SMP_MAX_CPUS + 31) / 32;
			const int32 cpuCount = smp_get_num_cpus();
			for (int32 i = 0; i < kCPUSetArraySize; i++) {
				uint32 bits = mask.Bits(i);
				while (bits != 0) {
					int bit = __builtin_ctz(bits);
					bits &= ~(1U << bit);
					int32 cpuID = i * 32 + bit;

					if (cpuID >= cpuCount)
						continue;

					core = CPUEntry::GetCPU(cpuID)->Core();
					if (core != NULL)
						break;
				}
				if (core != NULL && core->CPUMask().Matches(mask))
					break;
			}
		}
	}

	if (core == NULL)
		return NULL;

	ASSERT(core != NULL);

	CoreEntry* previousCore = threadData->PreviousCore();
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

	ASSERT(!gSingleCore);

	// Real-time threads bypass rebalancing to ensure zero jitter
	if (threadData->IsRealTime())
		return threadData->Core();

	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	CoreEntry* core = threadData->Core();
	ASSERT(core != NULL);

	int32 coreLoad = core->GetLoad();
	int32 cpuCount = core->CPUCount();
	int32 threadLoad = cpuCount > 0 ? threadData->GetLoad() / cpuCount : 0;
	if (coreLoad > kHighLoad) {
		if (atomic_pointer_get(&sSmallTaskCore) == core) {
			atomic_pointer_set(&sSmallTaskCore, (CoreEntry*)NULL);
			CoreEntry* smallTaskCore = choose_small_task_core();

			if (threadLoad > coreLoad / 3 || smallTaskCore == NULL
					|| (useMask && !smallTaskCore->CPUMask().Matches(mask))) {
				return core;
			}
			return coreLoad > kVeryHighLoad ? smallTaskCore : core;
		}

		if (threadLoad >= coreLoad >> 1)
			return core;

		CoreEntry* other = NULL;
		int32 bestLoad = -2;
		bool foundNonOverloaded = false;

		// Use random sampling if possible
		bool tryRandom = gPackageCount > kRandomSearchThreshold;

		if (tryRandom && !useMask) {
			// Phase 2: Local Node
			SchedulerNode* node = core->Package()->Node();
			search_local_node(node, [&](PackageEntry* entry) {
				check_package_packing(entry, NULL, other, bestLoad,
					foundNonOverloaded);
				return false;
			});

			// Phase 3: Global Random
			search_global_random([&](PackageEntry* entry) {
				check_package_packing(entry, NULL, other, bestLoad,
					foundNonOverloaded);
				return false;
			});

		} else if (useMask) {
			check_masked_packages_packing(mask, other, bestLoad, foundNonOverloaded);
		}

		if (other == NULL && !useMask) {
			// Phase 4: Limited Global Scan (Fallback)
			const int32 kMaxFallbackAttempts = 64;
			int32 startIndex = tryRandom ? CPUEntry::GetCPU(smp_get_current_cpu())->GetRandom() % gPackageCount : 0;
			int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

			for (int32 i = 0; i < attempts; i++) {
				int32 index = startIndex + i;
				if (index >= gPackageCount)
					index -= gPackageCount;
				check_package_packing(&gPackageEntries[index], NULL, other, bestLoad, foundNonOverloaded);
			}
		}

		// If other is NULL, we failed to find candidate.
		if (other == NULL) {
			if (core->CPUCount() == 0)
				return NULL; // Force migration to *any* core by triggering full search
			return core; // Fallback
		}
		ASSERT(other != NULL);

		// Advanced NUMA Support:
		// If the candidate core 'other' is in the thread's Home Package,
		// we reduce the migration threshold to encourage returning home.
		// Conversely, if 'other' is remote and we are currently home, we increase it.
		int32 homePackageID = threadData->HomePackage();
		int32 threshold = kLoadDifference >> 1;

		if (homePackageID >= 0) {
			int32 currentPackageID = core->Package()->ID();
			int32 otherPackageID = other->Package()->ID();

			if (otherPackageID == homePackageID && currentPackageID != homePackageID) {
				// Bonus for returning home
				threshold = 0;
			} else if (currentPackageID == homePackageID && otherPackageID != homePackageID) {
				// Penalty for leaving home
				threshold *= 2;
			}
		}

		int32 coreNewLoad = coreLoad - threadLoad;
		int32 otherNewLoad = other->GetLoad() + threadLoad;
		return coreNewLoad - otherNewLoad >= threshold ? other : core;
	}

	if (coreLoad >= kMediumLoad)
		return core;

	CoreEntry* smallTaskCore = choose_small_task_core();
	if (smallTaskCore == NULL || (useMask && !smallTaskCore->CPUMask().Matches(mask)))
		return core;
	return smallTaskCore->GetLoad() + threadLoad < kHighLoad
		? smallTaskCore : core;
}



static void
rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();

	CoreEntry* smallTaskCore = (CoreEntry*)atomic_pointer_get(&sSmallTaskCore);
	bool pack = idle && smallTaskCore != NULL;

	if (idle && !pack)
		return;

	if (!idle && smallTaskCore != NULL)
		return;

	cpu_ent* cpu = get_cpu_struct();
	if (pack && smallTaskCore == CoreEntry::GetCore(cpu->cpu_num))
		return;

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

	int32 chosenIRQ = -1;
	if (chosen != NULL)
		chosenIRQ = chosen->irq;

	locker.Unlock();

	if (chosen == NULL || (!pack && totalLoad < kLowLoad))
		return;

	CoreEntry* other = NULL;
	if (pack) {
		other = smallTaskCore;
	} else {
		int32 bestLoad = -2;

		// Use random sampling if possible
		bool tryRandom = gPackageCount > kRandomSearchThreshold;

		if (tryRandom) {
			// Phase 2: Local Node
			CoreEntry* currentCore = CoreEntry::GetCore(cpu->cpu_num);
			if (currentCore != NULL) {
				SchedulerNode* node = currentCore->Package()->Node();
				search_local_node(node, [&](PackageEntry* entry) {
					CheckPackageMinimumLoad(entry, NULL, other, bestLoad);
					return false;
				});
			}

			// Phase 3: Global Random
			search_global_random([&](PackageEntry* entry) {
				CheckPackageMinimumLoad(entry, NULL, other, bestLoad);
				return false;
			});
		}

		if (other == NULL) {
			// Limit fallback attempts
			const int32 kMaxFallbackAttempts = 64;
			int32 startIndex = tryRandom ? CPUEntry::GetCPU(smp_get_current_cpu())->GetRandom() % gPackageCount : 0;
			int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

			for (int32 i = 0; i < attempts; i++) {
				int32 index = startIndex + i;
				if (index >= gPackageCount)
					index -= gPackageCount;
				CheckPackageMinimumLoad(&gPackageEntries[index], NULL, other,
					bestLoad);
			}
		}
	}

	if (other == NULL)
		return;

	CoreCPUHeapLocker _(other);
	int32 newCPU = other->CPUHeap()->PeekRoot()->ID();
	_.Unlock();

	CoreEntry* core = CoreEntry::GetCore(smp_get_current_cpu());
	if (other == core)
		return;
	if (!pack && other->GetLoad() + kLoadDifference >= core->GetLoad())
		return;

	CPUEntry* cpuEntry = CPUEntry::GetCPU(cpu->cpu_num);
	cpuEntry->fRebalanceDPC.fIRQ = chosenIRQ;
	cpuEntry->fRebalanceDPC.fTargetCPU = newCPU;
	DPCQueue::DefaultQueue(B_NORMAL_PRIORITY)->Add(&cpuEntry->fRebalanceDPC);
}



scheduler_mode_operations gSchedulerPowerSavingMode = {
	"power saving",

	5000,
	1200,
	{ 3, 10 },

	20000,

	switch_to_mode,
	set_cpu_enabled,
	has_cache_expired,
	choose_core,
	rebalance,
	rebalance_irqs,
};
