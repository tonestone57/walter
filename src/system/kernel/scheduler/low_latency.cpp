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
	// use PreviousCore() instead of Core(). fCore is updated
	// by rebalance while the thread sleeps; fWentSleepActive was recorded
	// against the *old* core's active time. Comparing against the new core's
	// active time is meaningless and can incorrectly report cache-expired=false
	// for a thread that has migrated far away from its warm cache.
	CoreEntry* core = threadData->PreviousCore();
	if (core == NULL)
		return true; // no previous core — treat as expired
	bigtime_t activeTime = core->GetActiveTime();
	return activeTime - threadData->WentSleepActive() > kCacheExpire;
}






static CoreEntry*
choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
	CoreEntry* previousCore = threadData->PreviousCore();

	// has_cache_expired() calls system_time() internally.  It is
	// called up to three times in this function; cache the result to avoid
	// redundant syscalls and ensure a consistent view within one scheduling
	// decision.
	const bool cacheExpired = (previousCore != NULL)
		? has_cache_expired(threadData) : true;

	// Stage 0: Hot-Idle Fast Path
	// If the core we previously ran on is idle and in the same package,
	// use it immediately to preserve cache warmth and skip expensive search.
	// cpu->Core() can return NULL during hot-unplug; guard it.
	// Issue 32 fix: cpu->Core() can return NULL during hot-unplug; guard it.
	if (previousCore != NULL && previousCore->GetScore() == 0) {
		CoreEntry* currentCore = cpu->Core();
		if (currentCore != NULL
				&& previousCore->Package() == currentCore->Package())
			return previousCore;
	}

	// Try to use the previous core if it is idle and we have cache affinity.
	// We also try to use a core on the same package (L3 cache) or a sibling
	// core (L2 cache) to minimize cache misses.
	CPUSet mask = threadData->GetCPUMask();
	bool useMask = !mask.IsEmpty();

	// Thread Coloring: only meaningful on heterogeneous systems.
	// On homogeneous systems (gMinCoreType == gMaxCoreType) the type-filtered
	// search is equivalent to the general min-load search and adds overhead
	// without benefit. Skip it entirely.
	bool isForeground = threadData->IsForeground();
	int32 priority = threadData->GetPriority();
	bool preferMax = (priority > B_DISPLAY_PRIORITY || isForeground)
		&& (gMinCoreType != gMaxCoreType);
	bool preferMin = (priority < B_NORMAL_PRIORITY && !isForeground)
		&& (gMinCoreType != gMaxCoreType);

	// Optimization: If the mask is effectively "all enabled CPUs", treat it as no mask
	// to enable global random sampling instead of slow mask iteration.
	if (useMask && Scheduler::IsAllEnabledMask(mask))
		useMask = false;

	if (previousCore != NULL && !cacheExpired) {
		if (!useMask || previousCore->CPUMask().Matches(mask)) {
			// Respect thread coloring even for cache affinity
			bool typeMatch = true;
			if (preferMax && previousCore->Type() != gMaxCoreType)
				typeMatch = false;
			else if (preferMin && previousCore->Type() != gMinCoreType)
				typeMatch = false;

			if (typeMatch) {
				// Check if previous core is idle
				if (previousCore->GetLoad() == 0)
					return previousCore;

				// Check if previous core is lightly loaded (Soft Affinity)
				if (previousCore->GetScore() <= kLoadDifference)
					return previousCore;
			}
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

	// Thread Coloring: Search for a core of the preferred type first
	uint64 idleNodeMask = atomic_get64((int64*)&gIdleNodeMask);

	if (preferMax || preferMin) {
		CoreType preferredType = preferMax ? gMaxCoreType : gMinCoreType;

		// Try to find an idle core of the preferred type
		uint64 typeIdleNodeMask = idleNodeMask;
		while (typeIdleNodeMask != 0) {
			int32 nodeIndex = __builtin_ctzll(typeIdleNodeMask);
			typeIdleNodeMask &= ~(1ULL << nodeIndex);

			SchedulerNode* node = &gSchedulerNodes[nodeIndex];
			uint64 idlePackageMask = node->IdlePackageMask();

			while (idlePackageMask != 0) {
				int32 packageIndex = __builtin_ctzll(idlePackageMask);
				idlePackageMask &= ~(1ULL << packageIndex);

				int32 globalPackageIndex = node->PackageStartIndex() + packageIndex;
				if (globalPackageIndex >= gPackageCount)
					continue;

				PackageEntry* package = &gPackageEntries[globalPackageIndex];
				core = package->PeekMinimumLoadCore(cpu, &mask, preferredType);
				if (core != NULL && core->GetLoad() == 0)
					break;
				core = NULL;
			}
			if (core != NULL)
				break;
		}

		if (core == NULL) {
			// No idle core, try finding a lightly loaded one
			int32 bestScore = -1;
			bool tryRandom = gPackageCount > kRandomSearchThreshold;
			if (tryRandom && !useMask) {
				search_global_random([&](PackageEntry* entry) {
					return CheckPackageMinimumLoad(cpu, entry, NULL, core,
						bestScore, preferredType);
				});
			} else if (useMask) {
				CheckMaskedPackagesMinimumLoad(cpu, mask, core, bestScore,
					preferredType);
			}

			if (core == NULL && !useMask) {
				int32 startIndex = tryRandom
					? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
					: 0;
				int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

				for (int32 i = 0; i < attempts; i++) {
					int32 index = startIndex + i;
					if (index >= gPackageCount)
						index -= gPackageCount;
					if (CheckPackageMinimumLoad(cpu, &gPackageEntries[index], NULL,
						core, bestScore, preferredType))
					break;
				}
			}

			// use GetLoad() (raw utilisation) instead of GetScore()
			// (capacity-normalised) for the E-core guard. GetScore() scales by
			// 1/capacity, so an E-core at 10% raw load already exceeds kHighLoad
			// (~700) because its capacity is ~1/8th of a P-core. This made the
			// E-core coloring path immediately discard every E-core candidate,
			// rendering thread coloring for efficiency cores completely ineffective.
			if (preferMin && core != NULL && core->GetLoad() > kHighLoad)
				core = NULL;

			// For Performance cores, respect load threshold (80%).
			if (preferMax && core != NULL && core->GetLoad() > 800)
				core = NULL;
		}

		if (core != NULL) {
			// Optimization: If P-cores are moderately busy, allow the fallback
			// Phase 1 to check if a significantly less loaded Standard core
			// exists before committing to this P-core.
			if (!preferMax || core->GetScore() < kMediumLoad)
				return core;
		}

		// 3-type intermediate fallback: when P-cores are all overloaded and
		// STANDARD cores exist, try them before falling to the fully unfiltered
		// search (which might return an E-core for a high-priority thread).
		if (preferMax && gHasStandardCores) {
			int32 stdBestScore = -1;
			bool tryRandomStd = gPackageCount > kRandomSearchThreshold;

			if (tryRandomStd && !useMask) {
				search_global_random([&](PackageEntry* entry) {
					return CheckPackageMinimumLoad(cpu, entry, NULL, core,
						stdBestScore, CORE_TYPE_STANDARD);
				});
			} else if (useMask) {
				CheckMaskedPackagesMinimumLoad(cpu, mask, core, stdBestScore,
					CORE_TYPE_STANDARD);
			}

			if (core == NULL && !useMask) {
				int32 startIndex2 = tryRandomStd
					? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
					: 0;
				int32 attempts2 = min_c(gPackageCount, kMaxFallbackAttempts);
				for (int32 i = 0; i < attempts2; i++) {
					int32 index = startIndex2 + i;
					if (index >= gPackageCount)
						index -= gPackageCount;
					if (CheckPackageMinimumLoad(cpu, &gPackageEntries[index], NULL,
						core, stdBestScore, CORE_TYPE_STANDARD))
					break;
				}
			}

			if (core != NULL)
				return core;
		}
	}

	// If thread coloring found a result, do not enter the general
	// idle-node scan which iterates all idle nodes and can overwrite a valid
	// P-core selection with any idle core found (potentially an E-core).
	bool skipIdleScan = ((preferMax || preferMin) && core != NULL);

	// Respect skipIdleScan in the home-package check too.
	// The previous code entered the home-package path unconditionally.
	// By checking skipIdleScan we ensure that a valid colored core selection
	// is not overwritten.
	int32 homePackageID = threadData->HomePackage();
	if (!skipIdleScan && homePackageID >= 0 && homePackageID < gPackageCount) {
		PackageEntry* homePackage = &gPackageEntries[homePackageID];

		CoreType preferredType = preferMax ? gMaxCoreType :
			(preferMin ? gMinCoreType : CORE_TYPE_UNKNOWN);

		CoreEntry* candidate = homePackage->PeekMinimumLoadCore(cpu,
			useMask ? &mask : NULL, preferredType);

		if (candidate != NULL && candidate->GetLoad() == 0) {
			core = candidate;
		} else {
			// If no idle core in home package, check for lightly loaded one.
			// Pass NULL when !useMask: passing an all-zero CPUSet would cause
			// PeekMinimumLoadCore to reject every candidate, disabling the
			// NUMA home-package optimization for unconstrained threads.
			CoreEntry* bestHomeCore = NULL;
			int32 bestHomeLoad = -1;
			CheckPackageMinimumLoad(cpu, homePackage, useMask ? &mask : NULL,
				bestHomeCore, bestHomeLoad, preferredType);

			if (bestHomeCore != NULL && bestHomeLoad < kLoadDifference)
				core = bestHomeCore;
		}
	}

	// wake new package/core — skipped when thread coloring or home-package
	// search already found a suitable core.
	while (!skipIdleScan && idleNodeMask != 0) {
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
			native_cpu_mask_t idleMask = package->IdleCoreMask();
			while (idleMask != 0) {
				int32 bitIdx = scheduler_ctz(idleMask);
				idleMask &= ~((native_cpu_mask_t)1 << bitIdx);

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
				return CheckPackageMinimumLoad(cpu, entry, NULL, bestCore,
					bestLoad);
			});

			// Phase 3: Global Random
			search_global_random([&](PackageEntry* entry) {
				return CheckPackageMinimumLoad(cpu, entry, NULL, bestCore,
					bestLoad);
			});

		} else if (useMask) {
			CheckMaskedPackagesMinimumLoad(cpu, mask, bestCore, bestLoad);
		}

		// Fallback to full scan ONLY if we are not using random sampling (small system)
		// AND we didn't use mask iteration (handled above).
		// OR if random sampling failed completely to find *any* candidate (unlikely unless all broken).
		if (bestCore == NULL && !useMask) {
			// Limit fallback attempts to avoid O(N) scan on massive systems.
			// Start from a random index to ensure fairness over time.
			// 64 attempts cover small systems entirely and provide a reasonable
			// search depth for large ones.
			int32 startIndex = tryRandom
				? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
				: 0;
			int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

			// honour the return value of CheckPackageMinimumLoad.
			for (int32 i = 0; i < attempts; i++) {
				int32 index = startIndex + i;
				if (index >= gPackageCount)
					index -= gPackageCount;
				if (CheckPackageMinimumLoad(cpu, &gPackageEntries[index], NULL,
						bestCore, bestLoad))
					break;
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

	// If the selected core is not much better than previousCore, prefer
	// previousCore for cache locality.
	// Re-evaluate cache expiry here; the value computed at function
	// entry may be stale after hundreds of microseconds of search work.
	const bool cacheExpiredTail = (previousCore != NULL)
		? has_cache_expired(threadData) : true;
	if (previousCore != NULL && !cacheExpiredTail) {
		if (!useMask || previousCore->CPUMask().Matches(mask)) {
			if (core != previousCore) {
				// enforce type preference in the soft-affinity check
				// so an E-core previousCore is never returned for a P-coloured
				// thread just because loads are similar.
				bool typeOk = true;
				if (preferMax && previousCore->Type() != gMaxCoreType)
					typeOk = false;
				else if (preferMin && previousCore->Type() != gMinCoreType)
					typeOk = false;

				if (typeOk &&
					core->GetScore() + kLoadDifference >= previousCore->GetScore())
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

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
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
		SchedulerNode* node = NULL;
		if (core->Package() != NULL)
			node = core->Package()->Node();
		search_local_node(node, [&](PackageEntry* entry) {
			return CheckPackageMinimumLoad(cpu, entry, NULL, other, bestLoad);
		});

		// Phase 3: Global Random
		search_global_random([&](PackageEntry* entry) {
			return CheckPackageMinimumLoad(cpu, entry, NULL, other, bestLoad);
		});

	} else if (useMask) {
		CheckMaskedPackagesMinimumLoad(cpu, mask, other, bestLoad);
	}

	if (other == NULL && !useMask) {
		int32 startIndex = tryRandom
			? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
			: 0;
		int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

		// honour the return value of CheckPackageMinimumLoad.
		for (int32 i = 0; i < attempts; i++) {
			int32 index = startIndex + i;
			if (index >= gPackageCount)
				index -= gPackageCount;
			if (CheckPackageMinimumLoad(cpu, &gPackageEntries[index], NULL, other,
					bestLoad))
				break;
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

	int32 otherScore = other->GetScore();
	int32 coreScore = core->GetScore();

	if (other == core)
		return core;

	bigtime_t coreVRuntime = core->GetMinVirtualRuntime();
	bigtime_t otherVRuntime = other->GetMinVirtualRuntime();

	// If the current core is significantly lagging behind the other core,
	// we lower the threshold for migration to improve latency.
	// the "coreVRuntime > 0" guard prevents congestion detection
	// when all threads have just started (vruntime near zero). The meaningful
	// condition is whether the other core is ahead in virtual time; use an
	// absolute difference check instead.
	bool congested = otherVRuntime > coreVRuntime + 20000;

	// Heterogeneous Placement Stickiness: scale threshold by core performance
	int32 threshold = (kLoadDifference * core->PerformanceScale()) >> kDefaultCapacityShift;
	if (congested)
		threshold = 0;

	// Advanced NUMA Support:
	// If the candidate core 'other' is in the thread's Home Package,
	// we reduce the migration threshold to encourage returning home.
	// Conversely, if 'other' is remote and we are currently home, we increase it.
	int32 homePackageID = threadData->HomePackage();
	if (homePackageID >= 0 && core->Package() != NULL && other->Package() != NULL) {
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

	// Type-affinity guard: on heterogeneous systems, resist migrating a
	// thread off its preferred core type. choose_core places high-priority
	// threads on P-cores; without this guard rebalance undoes that on every
	// scheduling quantum by finding a lightly loaded E-core and moving the
	// thread there.
	//
	// We increase the migration threshold rather than blocking migration
	// entirely, so that severely overloaded P-cores can still shed load to
	// other types in extreme conditions.
	if (gMinCoreType != gMaxCoreType) {
		bool isFgRebal = threadData->IsForeground();
		int32 prioRebal = threadData->GetPriority();
		CoreType wantedType
			= (prioRebal > B_DISPLAY_PRIORITY || isFgRebal)
				? gMaxCoreType
				: (prioRebal < B_NORMAL_PRIORITY && !isFgRebal)
					? gMinCoreType
					: CORE_TYPE_UNKNOWN;

		if (wantedType != CORE_TYPE_UNKNOWN
				&& core->Type() == wantedType
				&& other->Type() != wantedType) {
			// Require a load difference twice the normal threshold before
			// accepting a cross-type migration.
			threshold = max_c(threshold, kLoadDifference * 2);
		}
	}

	if (otherScore + threshold >= coreScore)
		return core;

	// Check whether migrating the current thread would result in both core
	// loads become closer to the average.
	int32 difference = coreScore - otherScore - threshold;
	ASSERT(difference > 0);

	int32 cpuCount = core->CPUCount();
	int32 threadLoad = cpuCount > 0 ? threadData->GetLoad() / cpuCount : 0;

	// Check if migrating the thread would make the scores closer.
	// We compare the thread's weight on the current core vs its weight on the
	// target core to ensure the migration is truly beneficial on a normalized scale.
	int32 weightedLoadOnCore = ((int64)threadLoad * core->ScoreFactor()) >> 16;
	int32 weightedLoadOnOther = ((int64)threadLoad * other->ScoreFactor()) >> 16;

	int32 coreNewScore = coreScore - weightedLoadOnCore;
	int32 otherNewScore = otherScore + weightedLoadOnOther;

	return coreNewScore - otherNewScore >= threshold ? other : core;
}


static void
rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();

	if (idle)
		return;

	cpu_ent* cpu = get_cpu_struct();
	// Snapshot currentCore BEFORE releasing irqs_lock.  A
	// concurrent hot-unplug can change the CPU-to-core mapping in the window
	// between Unlock() and a second GetCore() call, producing a stale Package()
	// or Node() pointer.  This mirrors the identical fix applied to

	CoreEntry* currentCore = CoreEntry::GetCore(cpu->cpu_num);

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

	CPUEntry* cpuEntryForIRQ = CPUEntry::GetCPU(cpu->cpu_num);

	if (tryRandom) {
		// Phase 2: Local Node
		// Use the pre-lock snapshot .
		if (currentCore != NULL && currentCore->Package() != NULL) {
			SchedulerNode* node = currentCore->Package()->Node();
			search_local_node(node, [&](PackageEntry* entry) {
				return CheckPackageMinimumLoad(cpuEntryForIRQ, entry, NULL,
					other, bestLoad);
			});
		}

		// Phase 3: Global Random
		search_global_random([&](PackageEntry* entry) {
			return CheckPackageMinimumLoad(cpuEntryForIRQ, entry, NULL,
				other, bestLoad);
		});
	}

	// Use empty mask (NULL), as we don't care about affinity here
	if (other == NULL) {
		int32 startIndex = tryRandom
			? (int32)(((uint64)cpuEntryForIRQ->GetRandom() * gPackageCount) >> 32)
			: 0;
		int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

		// honour the return value of CheckPackageMinimumLoad.
		// It returns true when a near-idle core is found (<15% load),
		// signalling that further search is unnecessary. The original loops
		// always ran all kMaxFallbackAttempts iterations even after finding
		// an essentially-idle core.
		for (int32 i = 0; i < attempts; i++) {
			int32 index = startIndex + i;
			if (index >= gPackageCount)
				index -= gPackageCount;
			if (CheckPackageMinimumLoad(cpuEntryForIRQ, &gPackageEntries[index],
					NULL, other, bestLoad))
				break;
		}
	}

	if (other == NULL)
		return;

	CoreCPUHeapLocker _(other);
	int32 newCPU = other->CPUHeap()->PeekRoot()->ID();
	_.Unlock();

	// Use pre-lock snapshot; do NOT re-read via GetCore() here.
	if (other == currentCore)
		return;

	int32 otherLoad = other->GetScore();
	int32 coreLoad = currentCore->GetScore();

	if (otherLoad + kLoadDifference >= coreLoad)
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
