/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */


#include <util/AutoLock.h>
#include <util/Random.h>

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"
#include "scheduler_topology.h"


using namespace Scheduler;


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
	bool useMask = !mask.IsEmpty();

	// Optimization: If the mask is effectively "all enabled CPUs", treat it as no mask
	// to enable global random sampling instead of slow mask iteration.
	if (useMask && Scheduler::IsAllEnabledMask(mask))
		useMask = false;

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

			// Check local NUMA node (Super Package) to keep traffic local
			// This reduces interconnect saturation on large multi-socket systems.
			SchedulerNode* node = package->Node();
			if (node != NULL) {
				uint64 idlePackageMask = node->IdlePackageMask();
				while (idlePackageMask != 0) {
					int32 packageIndex = __builtin_ctzll(idlePackageMask);
					idlePackageMask &= ~(1ULL << packageIndex);

					int32 globalPackageIndex
						= node->PackageStartIndex() + packageIndex;
					if (globalPackageIndex >= gPackageCount)
						continue;

					PackageEntry* localPackage = &gPackageEntries[globalPackageIndex];
					CoreEntry* localSibling = localPackage->GetIdleCore();
					if (localSibling != NULL && (!useMask || localSibling->CPUMask().Matches(mask)))
						return localSibling;
				}
			}
		}
	}

	CoreEntry* core = NULL;

	// Check Home Package (NUMA/Memory Affinity)
	// If the previous core was not suitable (or cache expired), we try to return
	// the thread to its home package where its memory was likely allocated.
	int32 homePackageID = threadData->HomePackage();
	if (core == NULL && homePackageID >= 0 && homePackageID < gPackageCount) {
		PackageEntry* homePackage = &gPackageEntries[homePackageID];
		CoreEntry* candidate = homePackage->GetIdleCore();

		if (candidate != NULL) {
			if (!useMask || candidate->CPUMask().Matches(mask))
				core = candidate;
		} else {
			// If no idle core in home package, check for lightly loaded one
			CoreEntry* bestHomeCore = NULL;
			int32 bestHomeLoad = -1;
			CheckPackageMinimumLoad(homePackage, &mask, bestHomeCore, bestHomeLoad);

			if (bestHomeCore != NULL && bestHomeLoad < kLoadDifference)
				core = bestHomeCore;
		}
	}

	// wake new package/core
	uint64 idleNodeMask = atomic_get64((int64*)&gIdleNodeMask);
	while (idleNodeMask != 0) {
		int32 nodeIndex = __builtin_ctzll(idleNodeMask);
		idleNodeMask &= ~(1ULL << nodeIndex);

		SchedulerNode* node = &gSchedulerNodes[nodeIndex];
		uint64 idlePackageMask = node->IdlePackageMask();

		while (idlePackageMask != 0) {
			int32 packageIndex = __builtin_ctzll(idlePackageMask);
			idlePackageMask &= ~(1ULL << packageIndex);

			int32 globalPackageIndex
				= node->PackageStartIndex() + packageIndex;
			// Safety check for bounds, though masks shouldn't be set if out of bounds
			if (globalPackageIndex >= gPackageCount)
				continue;

			PackageEntry* package = &gPackageEntries[globalPackageIndex];
			uint32 idleMask = package->IdleCoreMask();
			while (idleMask != 0) {
				int32 bitIdx = __builtin_ctz(idleMask);
				idleMask &= ~(1U << bitIdx);

				CoreEntry* candidate = package->GetCore(bitIdx);
				if (!useMask || candidate->CPUMask().Matches(mask)) {
					core = candidate;
					break;
				}
			}
			if (core != NULL)
				break;
		}
		if (core != NULL)
			break;
	}

	if (core == NULL) {
		// no idle cores, search global packages for least occupied core
		CoreEntry* bestCore = NULL;
		int32 bestLoad = -1;

		// If we have many packages, use random sampling (Power of Two Choices)
		// to avoid the O(N) overhead of locking every package.
		bool tryRandom = gPackageCount > kRandomSearchThreshold;

		if (tryRandom && !useMask) {
			// Phase 2: Local Node
			SchedulerNode* node = NULL;
			if (previousCore != NULL)
				node = previousCore->Package()->Node();
			else if (homePackageID >= 0 && homePackageID < gPackageCount)
				node = gPackageEntries[homePackageID].Node();

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

		// Fallback to full scan ONLY if we are not using random sampling (small system)
		// AND we didn't use mask iteration (handled above).
		// OR if random sampling failed completely to find *any* candidate (unlikely unless all broken).
		if (bestCore == NULL && !useMask) {
			// Limit fallback attempts to avoid O(N) scan on massive systems.
			// Start from a random index to ensure fairness over time.
			// 64 attempts cover small systems entirely and provide a reasonable
			// search depth for large ones.
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
	}

	if (core == NULL) {
		core = CoreEntry::GetCore(smp_get_current_cpu());
		if (useMask && !core->CPUMask().Matches(mask)) {
			// fallback to the first valid core
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
	int32 bestLoad = -1;

	// Use random sampling if possible
	bool tryRandom = gPackageCount > kRandomSearchThreshold;

	if (tryRandom && !useMask) {
		// Phase 2: Local Node
		SchedulerNode* node = core->Package()->Node();
		search_local_node(node, [&](PackageEntry* entry) {
			CheckPackageMinimumLoad(entry, NULL, other, bestLoad);
			return false;
		});

		// Phase 3: Global Random
		search_global_random([&](PackageEntry* entry) {
			CheckPackageMinimumLoad(entry, NULL, other, bestLoad);
			return false;
		});

	} else if (useMask) {
		CheckMaskedPackagesMinimumLoad(mask, other, bestLoad);
	}

	if (other == NULL && !useMask) {
		const int32 kMaxFallbackAttempts = 64;
		int32 startIndex = tryRandom ? CPUEntry::GetCPU(smp_get_current_cpu())->GetRandom() % gPackageCount : 0;
		int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

		for (int32 i = 0; i < attempts; i++) {
			int32 index = startIndex + i;
			if (index >= gPackageCount)
				index -= gPackageCount;
			CheckPackageMinimumLoad(&gPackageEntries[index], NULL, other, bestLoad);
		}
	}

	if (other == NULL) {
		if (core->CPUCount() == 0)
			return NULL; // Force migration to *any* core by triggering full search
		return core; // Fallback
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

	// Advanced NUMA Support:
	// If the candidate core 'other' is in the thread's Home Package,
	// we reduce the migration threshold to encourage returning home.
	// Conversely, if 'other' is remote and we are currently home, we increase it.
	int32 homePackageID = threadData->HomePackage();
	if (homePackageID >= 0) {
		int32 currentPackageID = core->Package()->ID();
		int32 otherPackageID = other->Package()->ID();

		if (otherPackageID == homePackageID && currentPackageID != homePackageID) {
			// Bonus for returning home: effectively 0 threshold or even negative?
			// Let's just remove the friction (threshold).
			threshold = 0;
		} else if (currentPackageID == homePackageID && otherPackageID != homePackageID) {
			// Penalty for leaving home: double the threshold.
			threshold *= 2;
		}
	}

	if (otherLoad + threshold >= coreLoad)
		return core;

	// Check whether migrating the current thread would result in both core
	// loads become closer to the average.
	int32 difference = coreLoad - otherLoad - threshold;
	ASSERT(difference > 0);

	int32 cpuCount = core->CPUCount();
	int32 threadLoad = cpuCount > 0 ? threadData->GetLoad() / cpuCount : 0;
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

	int32 chosenIRQ = -1;
	if (chosen != NULL)
		chosenIRQ = chosen->irq;

	locker.Unlock();

	if (chosen == NULL || totalLoad < kLowLoad)
		return;

	CoreEntry* other = NULL;
	int32 bestLoad = -1;

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

	// Use empty mask (NULL), as we don't care about affinity here
	if (other == NULL) {
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

	if (other == NULL)
		return;

	CoreCPUHeapLocker _(other);
	int32 newCPU = other->CPUHeap()->PeekRoot()->ID();
	_.Unlock();

	CoreEntry* core = CoreEntry::GetCore(cpu->cpu_num);
	if (other == core)
		return;
	if (other->GetLoad() + kLoadDifference >= core->GetLoad())
		return;

	CPUEntry* cpuEntry = CPUEntry::GetCPU(cpu->cpu_num);
	cpuEntry->fRebalanceDPC.fIRQ = chosenIRQ;
	cpuEntry->fRebalanceDPC.fTargetCPU = newCPU;
	DPCQueue::DefaultQueue(B_NORMAL_PRIORITY)->Add(&cpuEntry->fRebalanceDPC);
}


scheduler_mode_operations gSchedulerLowLatencyMode = {
	"low latency",

	1600,
	1200,
	{ 2, 5 },

	3200,

	switch_to_mode,
	set_cpu_enabled,
	has_cache_expired,
	choose_core,
	rebalance,
	rebalance_irqs,
};
