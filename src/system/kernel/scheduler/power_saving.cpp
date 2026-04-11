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


static CoreEntry** sSmallTaskCore;


static void
switch_to_mode()
{
	if (sSmallTaskCore == NULL)
		sSmallTaskCore = new(std::nothrow) CoreEntry*[gNodeCount]();
	if (sSmallTaskCore != NULL) {
		for (int32 i = 0; i < gNodeCount; i++)
			atomic_pointer_set(&sSmallTaskCore[i], (CoreEntry*)NULL);
	}
}


static void
set_cpu_enabled(int32 cpu, bool enabled)
{
	if (!enabled && sSmallTaskCore != NULL) {
		for (int32 i = 0; i < gNodeCount; i++)
			atomic_pointer_set(&sSmallTaskCore[i], (CoreEntry*)NULL);
	}
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
check_package_small_task(PackageEntry* entry, CoreEntry*& core, int32& bestScore)
{
	// Find the core with highest score that isn't overloaded.
	// If all are overloaded, we might pick the least loaded later.
	// For choosing a small task core, we want packing.
	CoreEntry* candidate = entry->PeekMaximumLoadCore();

	if (candidate != NULL) {
		if (candidate->GetScore() >= kHighLoad) {
			// The busiest is overloaded. Check if there is a less loaded one.
			candidate = entry->PeekMinimumLoadCore();
		}
	}

	if (candidate != NULL) {
		int32 score = candidate->GetScore();
		if (core == NULL) {
			core = candidate;
			bestScore = score;
		} else {
			bool bestOverloaded = bestScore >= kHighLoad;
			bool candidateOverloaded = score >= kHighLoad;

			if (candidateOverloaded) {
				// If candidate is overloaded, we only pick it if current is ALSO overloaded
				// AND candidate is LESS loaded (minimize overload).
				if (bestOverloaded && score < bestScore) {
					core = candidate;
					bestScore = score;
				}
			} else {
				// Candidate is NOT overloaded.
				// If current is overloaded, we definitely switch.
				// If current is NOT overloaded, we switch if candidate is BUSIER (packing).
				if (bestOverloaded || score > bestScore) {
					core = candidate;
					bestScore = score;
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
	int32 bestScore = -1;

	bool tryRandom = gPackageCount > kRandomSearchThreshold;
	if (tryRandom) {
		search_global_random([&](PackageEntry* entry) {
			check_package_small_task(entry, core, bestScore);
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
			check_package_small_task(&gPackageEntries[index], core, bestScore);
		}
	}

	if (core == NULL)
		return NULL;

	if (sSmallTaskCore != NULL) {
		int32 nodeID = core->Package()->Node()->ID();
		CoreEntry* smallTaskCore
			= (CoreEntry*)atomic_pointer_test_and_set(&sSmallTaskCore[nodeID], core,
				(CoreEntry*)NULL);
		if (smallTaskCore == NULL)
			return core;
		return smallTaskCore;
	}
	return core;
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
	CoreEntry*& other, int32& bestScore, bool& foundNonOverloaded,
	CoreType type = CORE_TYPE_UNKNOWN)
{
	// We want to pack: find the busiest core that is NOT overloaded (score < kHighLoad).
	// If all active cores are overloaded, pick the least loaded one (to minimize overload).
	CoreEntry* candidate = entry->PeekMaximumLoadCore(mask, type);

	if (candidate != NULL) {
		if (candidate->GetScore() >= kHighLoad) {
			// The busiest is overloaded. Check if there is a less loaded one.
			candidate = entry->PeekMinimumLoadCore(mask, type);
		}
	}

	if (candidate != NULL) {
		int32 score = candidate->GetScore();
		bool isOverloaded = score >= kHighLoad;

		if (other == NULL) {
			other = candidate;
			bestScore = score;
			foundNonOverloaded = !isOverloaded;
		} else if (foundNonOverloaded) {
			if (!isOverloaded) {
				// Both are non-overloaded. Pick the BUSIEST (packing).
				if (score > bestScore) {
					other = candidate;
					bestScore = score;
				}
			}
			// If candidate is overloaded, ignore it (we prefer the existing non-overloaded 'other')
		} else {
			if (!isOverloaded) {
				// Found a non-overloaded core! It beats the current overloaded 'other'.
				other = candidate;
				bestScore = score;
				foundNonOverloaded = true;
			} else {
				// Both are overloaded. Pick the LEAST loaded (spread overload).
				if (score < bestScore) {
					other = candidate;
					bestScore = score;
				}
			}
		}
	}
}


static void
check_masked_packages_packing(const CPUSet& mask, CoreEntry*& other,
	int32& bestScore, bool& foundNonOverloaded, CoreType type = CORE_TYPE_UNKNOWN)
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
					check_package_packing(package, &mask, other, bestScore,
						foundNonOverloaded, type);
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

	// Thread Coloring: High-priority threads prefer P-cores
	bool isForeground = threadData->IsForeground();
	bool preferP = threadData->GetPriority() > B_DISPLAY_PRIORITY || isForeground;
	bool preferE = threadData->GetPriority() < B_NORMAL_PRIORITY && !isForeground;

	// Optimization: If the mask is effectively "all enabled CPUs", treat it as no mask
	if (useMask && Scheduler::IsAllEnabledMask(mask))
		useMask = false;

	// Thread Coloring: Search for a core of the preferred type first
	if (preferP || preferE) {
		CoreType preferredType = preferP ? CORE_TYPE_PERFORMANCE : CORE_TYPE_EFFICIENCY;
		int32 bestScore = -1;
		bool foundNonOverloaded = false;
		bool tryRandom = gPackageCount > kRandomSearchThreshold;

		if (tryRandom && !useMask) {
			search_global_random([&](PackageEntry* entry) {
				check_package_packing(entry, NULL, core, bestScore,
					foundNonOverloaded, preferredType);
				return false;
			});
		} else if (useMask) {
			check_masked_packages_packing(mask, core, bestScore,
				foundNonOverloaded, preferredType);
		}

		if (core == NULL) {
			const int32 kMaxFallbackAttempts = 64;
			int32 startIndex = tryRandom ? CPUEntry::GetCPU(smp_get_current_cpu())->GetRandom() % gPackageCount : 0;
			int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

			for (int32 i = 0; i < attempts; i++) {
				int32 index = startIndex + i;
				if (index >= gPackageCount)
					index -= gPackageCount;
				check_package_packing(&gPackageEntries[index], NULL, core,
					bestScore, foundNonOverloaded, preferredType);
			}
		}

		// For P-cores, respect load threshold (80%).
		if (preferP && core != NULL && core->GetLoad() > 800)
			core = NULL;

		if (core != NULL)
			return core;
	}

	// try to pack all threads on one core
	core = choose_small_task_core();
	if (core != NULL && (useMask && !core->CPUMask().Matches(mask)))
		core = NULL;

	if (core == NULL || core->GetScore() + threadData->GetLoad() >= kHighLoad) {
		// run immediately on already woken core
		CoreEntry* bestCore = NULL;
		int32 bestScore = -1;

		bool tryRandom = gPackageCount > kRandomSearchThreshold;

		if (tryRandom && !useMask) {
			CoreEntry* previousCore = threadData->PreviousCore();

			// Phase 1: L3 Domain (Sibling in previous package)
			if (previousCore != NULL && !has_cache_expired(threadData)) {
				PackageEntry* package = previousCore->Package();
				if (package != NULL) {
					CheckPackageMinimumLoad(package, NULL, bestCore, bestScore);
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
				CheckPackageMinimumLoad(entry, NULL, bestCore, bestScore);
				return false;
			});

			// Phase 3: Global Random
			search_global_random([&](PackageEntry* entry) {
				CheckPackageMinimumLoad(entry, NULL, bestCore, bestScore);
				return false;
			});

		} else if (useMask) {
			CheckMaskedPackagesMinimumLoad(mask, bestCore, bestScore);
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
					bestScore);
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

	int32 coreScore = core->GetScore();
	int32 cpuCount = core->CPUCount();
	int32 threadLoad = cpuCount > 0 ? threadData->GetLoad() / cpuCount : 0;
	if (coreScore > kHighLoad) {
		int32 nodeID = core->Package()->Node()->ID();
		if (sSmallTaskCore != NULL && atomic_pointer_get(&sSmallTaskCore[nodeID]) == core) {
			atomic_pointer_set(&sSmallTaskCore[nodeID], (CoreEntry*)NULL);
			CoreEntry* smallTaskCore = choose_small_task_core();

			if (threadLoad > coreScore / 3 || smallTaskCore == NULL
					|| (useMask && !smallTaskCore->CPUMask().Matches(mask))) {
				return core;
			}
			return coreScore > kVeryHighLoad ? smallTaskCore : core;
		}

		if (threadLoad >= coreScore >> 1)
			return core;

		CoreEntry* other = NULL;
		int32 bestScore = -2;
		bool foundNonOverloaded = false;

		// Use random sampling if possible
		bool tryRandom = gPackageCount > kRandomSearchThreshold;

		if (tryRandom && !useMask) {
			// Phase 2: Local Node
			SchedulerNode* node = core->Package()->Node();
			search_local_node(node, [&](PackageEntry* entry) {
				check_package_packing(entry, NULL, other, bestScore,
					foundNonOverloaded);
				return false;
			});

			// Phase 3: Global Random
			search_global_random([&](PackageEntry* entry) {
				check_package_packing(entry, NULL, other, bestScore,
					foundNonOverloaded);
				return false;
			});

		} else if (useMask) {
			check_masked_packages_packing(mask, other, bestScore, foundNonOverloaded);
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
				check_package_packing(&gPackageEntries[index], NULL, other,
					bestScore, foundNonOverloaded);
			}
		}

		// If other is NULL, we failed to find candidate.
		if (other == NULL) {
			if (core->CPUCount() == 0)
				return NULL; // Force migration to *any* core by triggering full search
			return core; // Fallback
		}
		ASSERT(other != NULL);

		int32 coreNewScore = coreScore - threadLoad;
		int32 otherNewScore = other->GetScore() + threadLoad;
		return coreNewScore - otherNewScore >= kLoadDifference >> 1 ? other : core;
	}

	if (coreScore >= kMediumLoad)
		return core;

	int32 nodeID = core->Package()->Node()->ID();
	CoreEntry* smallTaskCore = choose_small_task_core();
	if (smallTaskCore == NULL || (useMask && !smallTaskCore->CPUMask().Matches(mask)))
		return core;
	return smallTaskCore->GetScore() + threadLoad < kHighLoad
		? smallTaskCore : core;
}


static void
rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();

	bool pack = false;
	bool hasSmallTaskCore = false;
	if (sSmallTaskCore != NULL) {
		for (int32 i = 0; i < gNodeCount; i++) {
			if (atomic_pointer_get(&sSmallTaskCore[i]) != NULL) {
				hasSmallTaskCore = true;
				break;
			}
		}
	}
	pack = idle && hasSmallTaskCore;

	if (idle) {
		if (!pack)
			return;
	} else {
		if (hasSmallTaskCore)
			return;
	}

	cpu_ent* cpu = get_cpu_struct();
	CoreEntry* currentCore = CoreEntry::GetCore(cpu->cpu_num);

	if (pack && sSmallTaskCore != NULL && currentCore != NULL) {
		int32 nodeID = currentCore->Package()->Node()->ID();
		if (atomic_pointer_get(&sSmallTaskCore[nodeID]) == currentCore)
			return;
	}

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
		if (sSmallTaskCore != NULL && currentCore != NULL) {
			int32 nodeID = currentCore->Package()->Node()->ID();
			other = (CoreEntry*)atomic_pointer_get(&sSmallTaskCore[nodeID]);
		}
	} else {
		int32 bestScore = -2;

		// Use random sampling if possible
		bool tryRandom = gPackageCount > kRandomSearchThreshold;

		if (tryRandom) {
			// Phase 2: Local Node
			currentCore = CoreEntry::GetCore(cpu->cpu_num);
			if (currentCore != NULL) {
				SchedulerNode* node = currentCore->Package()->Node();
				search_local_node(node, [&](PackageEntry* entry) {
					CheckPackageMinimumLoad(entry, NULL, other, bestScore);
					return false;
				});
			}

			// Phase 3: Global Random
			search_global_random([&](PackageEntry* entry) {
				CheckPackageMinimumLoad(entry, NULL, other, bestScore);
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
					bestScore);
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
	if (!pack && other->GetScore() + kLoadDifference >= core->GetScore())
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
