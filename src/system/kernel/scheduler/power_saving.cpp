/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 * Audit fixes applied 2025.
 */

#include <util/AutoLock.h>
#include <util/Random.h>
#include <util/atomic.h>

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"
#include "scheduler_topology.h"

namespace Scheduler {

static void check_package_small_task(CPUEntry* cpu, PackageEntry* entry,
									 CoreEntry*& core, int32& bestScore);

static void check_package_packing(CPUEntry* cpu, PackageEntry* entry,
								  const CPUSet* mask, CoreEntry*& other,
								  int32& bestScore, bool& foundNonOverloaded,
								  CoreType type = CORE_TYPE_UNKNOWN);

struct MinimumLoadAction {
	CPUEntry* cpu;
	const CPUSet* mask;
	CoreEntry*& bestCore;
	int32& bestLoad;
	CoreType type;

	MinimumLoadAction(CPUEntry* c, const CPUSet* m, CoreEntry*& bc, int32& bl,
					  CoreType t = CORE_TYPE_UNKNOWN)
		: cpu(c), mask(m), bestCore(bc), bestLoad(bl), type(t) {}

	bool operator()(PackageEntry* entry) const {
		return CheckPackageMinimumLoad(cpu, entry, mask, bestCore, bestLoad,
									   type);
	}
};

struct SmallTaskAction {
	CPUEntry* cpu;
	CoreEntry*& core;
	int32& bestScore;

	SmallTaskAction(CPUEntry* c, CoreEntry*& co, int32& s)
		: cpu(c), core(co), bestScore(s) {}

	bool operator()(PackageEntry* entry) const {
		check_package_small_task(cpu, entry, core, bestScore);
		return core != NULL && bestScore >= (kHighLoad * 3) / 4;
	}
};

struct ECoreSmallTaskAction {
	CPUEntry* cpu;
	CoreEntry*& eCore;
	int32& eBestScore;

	ECoreSmallTaskAction(CPUEntry* c, CoreEntry*& ec, int32& es)
		: cpu(c), eCore(ec), eBestScore(es) {}

	bool operator()(PackageEntry* entry) const {
		CoreEntry* candidate =
			entry->PeekMaximumLoadCore(cpu, NULL, gMinCoreType);
		if (candidate != NULL && candidate->GetScore() < kHighLoad) {
			int32 score = candidate->GetScore();
			if (eCore == NULL || score > eBestScore) {
				eCore = candidate;
				eBestScore = score;
			}
		}
		return eCore != NULL && eBestScore >= (kHighLoad * 3) / 4;
	}
};

struct PackagePackingAction {
	CPUEntry* cpu;
	const CPUSet* mask;
	CoreEntry*& other;
	int32& bestScore;
	bool& foundNonOverloaded;
	CoreType type;

	PackagePackingAction(CPUEntry* c, const CPUSet* m, CoreEntry*& o, int32& bs,
						 bool& fno, CoreType t = CORE_TYPE_UNKNOWN)
		: cpu(c),
		  mask(m),
		  other(o),
		  bestScore(bs),
		  foundNonOverloaded(fno),
		  type(t) {}

	bool operator()(PackageEntry* entry) const {
		check_package_packing(cpu, entry, mask, other, bestScore,
							  foundNonOverloaded, type);
		return other != NULL && foundNonOverloaded &&
			   bestScore >= (kHighLoad * 3) / 4;
	}
};

// --- Scheduler tuning (power saving mode improvements) ---
static const int kConsolidationThreshold = 2;
static const int kMaxCPUsToScan = 8;
static const bigtime_t kIdleConsolidationCooldown __attribute__((aligned(8))) =
	2000;

static CoreEntry** sSmallTaskCore;

static void switch_to_mode() {
	if (sSmallTaskCore == NULL) {
		sSmallTaskCore = new (std::nothrow) CoreEntry*[gNodeCount];
		if (sSmallTaskCore != NULL)
			memset(sSmallTaskCore, 0, sizeof(CoreEntry*) * gNodeCount);
	}
	if (sSmallTaskCore != NULL) {
		for (int32 i = 0; i < gNodeCount; i++)
			atomic_pointer_set<CoreEntry>(&sSmallTaskCore[i], (CoreEntry*)NULL);
	}
}

static void set_cpu_enabled(int32 cpu, bool enabled) {
	if (!enabled && sSmallTaskCore != NULL) {
		for (int32 i = 0; i < gNodeCount; i++)
			atomic_pointer_set<CoreEntry>(&sSmallTaskCore[i], (CoreEntry*)NULL);
	}
}

static bool has_cache_expired(const ThreadData* threadData, bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();
	if (threadData->WentSleepActive() == 0)
		return false;
	// Note: (power_saving): same as low_latency - use PreviousCore().
	CoreEntry* core = threadData->PreviousCore();
	if (core == NULL)
		return true;
	bigtime_t activeTime = core->GetActiveTime();
	return activeTime - threadData->WentSleepActive() > kCacheExpire;
}

static void check_package_small_task(CPUEntry* cpu, PackageEntry* entry,
									 CoreEntry*& core, int32& bestScore) {
	// Find the core with highest score that isn't overloaded.
	// If all are overloaded, we might pick the least loaded later.
	// For choosing a small task core, we want packing.
	CoreEntry* candidate = entry->PeekMaximumLoadCore(cpu);

	if (candidate != NULL) {
		if (candidate->GetScore() >= kHighLoad) {
			// The busiest is overloaded. Check if there is a less loaded one.
			candidate = entry->PeekMinimumLoadCore(cpu);
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
				// If candidate is overloaded, we only pick it if current is
				// ALSO overloaded AND candidate is LESS loaded (minimize
				// overload).
				if (bestOverloaded && score < bestScore) {
					core = candidate;
					bestScore = score;
				}
			} else {
				// Candidate is NOT overloaded.
				// If current is overloaded, we definitely switch.
				// If current is NOT overloaded, we switch if candidate is
				// BUSIER (packing). Added hysteresis (kLoadDifference / 4) to
				// prevent ping-ponging.
				if (bestOverloaded ||
					score > bestScore + (kLoadDifference >> 2)) {
					core = candidate;
					bestScore = score;
				}
			}
		}
	}
}

static CoreEntry* choose_small_task_core(CPUEntry* cpu) {
	SCHEDULER_ENTER_FUNCTION();

	// Note: cpu->Core() can return NULL during hot-unplug.
	// Guard ALL dereferences of cpu->Core()->Package()->Node() up front
	// since both the heterogeneous and homogeneous paths use this chain.
	CoreEntry* cpuCore = cpu->Core();
	if (cpuCore == NULL || cpuCore->Package() == NULL ||
		cpuCore->Package()->Node() == NULL) {
		return NULL;
	}

	// On heterogeneous systems, small-task packing should prefer E-cores
	// (gMinCoreType) to keep P-cores available for high-priority foreground
	// work. On homogeneous systems packType is UNKNOWN and we fall through to
	// the general packing logic.
	if (sSmallTaskCore != NULL && gMinCoreType != gMaxCoreType) {
		int32 currentNodeID = cpuCore->Package()->Node()->ID();
		if (currentNodeID < 0 || currentNodeID >= gNodeCount)
			return NULL;

		CoreEntry* current = (CoreEntry*)atomic_pointer_get<CoreEntry>(
			&sSmallTaskCore[currentNodeID]);
		if (current != NULL && current->Type() == gMinCoreType &&
			current->GetScore() < kHighLoad) {
			return current;
		}

		CoreEntry* eCore = NULL;
		int32 eBestScore = -1;

		bool tryRandom = gPackageCount > kRandomSearchThreshold;
		if (tryRandom) {
			search_global_random(ECoreSmallTaskAction(cpu, eCore, eBestScore));
		}

		if (eCore == NULL) {
			int32 start =
				tryRandom
					? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
					: 0;
			for (int32 i = 0; i < min_c(gPackageCount, kMaxFallbackAttempts);
				 i++) {
				int32 idx = start + i;
				if (idx >= gPackageCount)
					idx -= gPackageCount;
				CoreEntry* candidate = gPackageEntries[idx].PeekMaximumLoadCore(
					cpu, NULL, gMinCoreType);
				if (candidate != NULL && candidate->GetScore() < kHighLoad) {
					int32 score = candidate->GetScore();
					if (eCore == NULL || score > eBestScore) {
						eCore = candidate;
						eBestScore = score;
					}
				}
			}
		}

		// If a suitable E-core was found, pin sSmallTaskCore to it and return.
		// Fall through to the general packing logic only when no E-core has
		// capacity (all E-cores are already heavily loaded).
		if (eCore != NULL) {
			int32 nodeID = eCore->Package()->Node()->ID();
			if (nodeID < 0 || nodeID >= gNodeCount)
				return eCore;

			int casRetries = 0;
			const int kMaxCASRetries = 16;
			while (true) {
				CoreEntry* currentE =
					atomic_pointer_get<CoreEntry>(&sSmallTaskCore[nodeID]);
				if (currentE != NULL && currentE->Type() == gMinCoreType &&
					currentE->GetScore() < kHighLoad) {
					return currentE;
				}

				if (atomic_pointer_test_and_set<CoreEntry>(
						&sSmallTaskCore[nodeID], eCore, currentE) == currentE) {
					return eCore;
				}

				if (++casRetries >= kMaxCASRetries) {
					CoreEntry* latest =
						atomic_pointer_get<CoreEntry>(&sSmallTaskCore[nodeID]);
					return (latest != NULL) ? latest : eCore;
				}
				cpu_pause();
			}
		}
	}

	CoreEntry* core = NULL;
	int32 bestScore = -1;

	if (sSmallTaskCore != NULL) {
		int32 currentNodeID = cpuCore->Package()->Node()->ID();
		if (currentNodeID < 0 || currentNodeID >= gNodeCount)
			return NULL;

		CoreEntry* current = (CoreEntry*)atomic_pointer_get<CoreEntry>(
			&sSmallTaskCore[currentNodeID]);
		if (current != NULL && current->GetScore() < kHighLoad)
			return current;
	}

	bool tryRandom = gPackageCount > kRandomSearchThreshold;
	if (tryRandom) {
		search_global_random(SmallTaskAction(cpu, core, bestScore));
	}

	// Fallback to full scan if random sampling failed to find a candidate
	// or if system is small.
	if (core == NULL) {
		// Use the global kMaxFallbackAttempts constant from scheduler_common.h.
		int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);
		int32 startIndex =
			tryRandom
				? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
				: 0;

		for (int32 i = 0; i < attempts; i++) {
			int32 index = startIndex + i;
			if (index >= gPackageCount)
				index -= gPackageCount;
			check_package_small_task(cpu, &gPackageEntries[index], core,
									 bestScore);
		}
	}

	if (core == NULL)
		return NULL;

	// CheckPackageMinimumLoad can return cores from packages whose
	// Init() was partially skipped (Package() or Node() is NULL).  Guard
	// before the sSmallTaskCore update which dereferences both.
	if (sSmallTaskCore != NULL && core != NULL) {
		if (core->Package() == NULL || core->Package()->Node() == NULL)
			return core;  // safe to use, just skip cache update
	}

	if (sSmallTaskCore != NULL) {
		int32 nodeID = core->Package()->Node()->ID();
		if (nodeID < 0 || nodeID >= gNodeCount)
			return core;

		// Note: add bounded retry with exponential backoff to prevent
		// busy-spinning under high contention from many CPUs simultaneously
		// discovering a new small-task core candidate.
		int casRetries = 0;
		const int kMaxCASRetries = 16;
		while (true) {
			CoreEntry* current =
				atomic_pointer_get<CoreEntry>(&sSmallTaskCore[nodeID]);
			if (current != NULL && current->GetScore() < kHighLoad)
				return current;

			if (atomic_pointer_test_and_set<CoreEntry>(
					&sSmallTaskCore[nodeID], core, current) == current) {
				return core;
			}

			if (++casRetries >= kMaxCASRetries) {
				// Give up; return best known candidate to avoid spinning.
				CoreEntry* latest =
					atomic_pointer_get<CoreEntry>(&sSmallTaskCore[nodeID]);
				return (latest != NULL) ? latest : core;
			}
			cpu_pause();
		}
	}
	return core;
}

static CoreEntry* choose_idle_core(CPUEntry* cpu, const CPUSet* mask = NULL) {
	SCHEDULER_ENTER_FUNCTION();

	PackageEntry* package = PackageEntry::GetLeastIdlePackage();

	if (package == NULL) {
		// No partially idle packages. Check for any idle package using the
		// mask.
		uint64 idleNodeMask = LoadAcquire64(gIdleNodeMask);
		int scannedCount = 0;
		while (idleNodeMask != 0) {
			if (++scannedCount > kMaxCPUsToScan)
				break;

			int32 nodeIndex = scheduler_ffs64(idleNodeMask) - 1;
			idleNodeMask &= ~(1ULL << nodeIndex);

			SchedulerNode* node = &gSchedulerNodes[nodeIndex];
			uint64 idlePackageMask = node->IdlePackageMask();

			if (idlePackageMask != 0) {
				int32 packageIndex = scheduler_ffs64(idlePackageMask) - 1;
				// fPackageStartIndex + packageIndex gives global index
				int32 globalIndex = node->PackageStartIndex() + packageIndex;
				// Note: added missing gPackageCount guard.
				if (globalIndex < gPackageCount) {
					PackageEntry* candidate = &gPackageEntries[globalIndex];
					// Note: check if package has any idle cores.
					if (candidate->IdleCoreCount() > 0) {
						package = candidate;
						break;
					}
				}
			}
		}
	}

	if (package != NULL)
		return package->GetIdleCorePacking(cpu, mask);
	return NULL;
}

static void check_package_packing(CPUEntry* cpu, PackageEntry* entry,
								  const CPUSet* mask, CoreEntry*& other,
								  int32& bestScore, bool& foundNonOverloaded,
								  CoreType type = CORE_TYPE_UNKNOWN) {
	// We want to pack: find the busiest core that is NOT overloaded (score <
	// kHighLoad). If all active cores are overloaded, pick the least loaded one
	// (to minimize overload).
	CoreEntry* candidate = entry->PeekMaximumLoadCore(cpu, mask, type);

	if (candidate != NULL) {
		if (candidate->GetScore() >= kHighLoad) {
			// The busiest is overloaded. Check if there is a less loaded one.
			candidate = entry->PeekMinimumLoadCore(cpu, mask, type);
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
			// If candidate is overloaded, ignore it (we prefer the existing
			// non-overloaded 'other')
		} else {
			if (!isOverloaded) {
				// Found a non-overloaded core! It beats the current overloaded
				// 'other'.
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

static void check_masked_packages_packing(CPUEntry* cpu, const CPUSet& mask,
										  CoreEntry*& other, int32& bestScore,
										  bool& foundNonOverloaded,
										  CoreType type = CORE_TYPE_UNKNOWN) {
	const int32 kCPUSetArraySize = (SMP_MAX_CPUS + 31) / 32;
	const int32 cpuCount = smp_get_num_cpus();
	PackageEntry* lastPackage = NULL;

	for (int32 i = 0; i < kCPUSetArraySize; i++) {
		uint32 bits = mask.Bits(i);
		if (bits == 0)
			continue;

		while (bits != 0) {
			int bit = scheduler_ctz((native_cpu_mask_t)bits);
			bits &= ~(1U << bit);
			int32 cpuID = i * 32 + bit;

			if (cpuID >= cpuCount)
				continue;

			CoreEntry* cpuCore = CPUEntry::GetCPU(cpuID)->Core();
			if (cpuCore != NULL) {
				PackageEntry* package = cpuCore->Package();
				if (package != NULL && package != lastPackage) {
					check_package_packing(cpu, package, &mask, other, bestScore,
										  foundNonOverloaded, type);
					lastPackage = package;
				}
			}
		}
	}
}

static CoreEntry* choose_core(const ThreadData* threadData, const CPUSet& mask,
							  bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (now == 0)
		now = system_time();

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
	CoreEntry* core = NULL;

	bool useMask = !mask.IsEmpty();

	// Optimization: Treat "all enabled" mask as no mask to enable fast sampling
	if (useMask && Scheduler::IsAllEnabledMask(mask))
		useMask = false;

	// Thread Coloring: only meaningful on heterogeneous systems.
	// Skip on homogeneous systems where gMinCoreType == gMaxCoreType to avoid
	// a redundant type-filtered search that returns the same result as the
	// general packing logic below.
	bool isForeground = threadData->IsForeground();
	int32 priority = threadData->GetPriority();
	bool preferMax = (priority > B_DISPLAY_PRIORITY || isForeground) &&
					 (gMinCoreType != gMaxCoreType);
	bool preferMin = (priority < B_NORMAL_PRIORITY && !isForeground) &&
					 (gMinCoreType != gMaxCoreType);

	// Thread Coloring: Search for a core of the preferred type first
	if (preferMax || preferMin) {
		CoreType preferredType = preferMax ? gMaxCoreType : gMinCoreType;
		int32 bestScore = -1;
		bool foundNonOverloaded = false;
		bool tryRandom = gPackageCount > kRandomSearchThreshold;

		if (tryRandom && !useMask) {
			search_global_random(PackagePackingAction(
				cpu, NULL, core, bestScore, foundNonOverloaded, preferredType));
		} else if (useMask) {
			check_masked_packages_packing(cpu, mask, core, bestScore,
										  foundNonOverloaded, preferredType);
		}

		if (core == NULL && !useMask) {
			int32 startIndex =
				tryRandom
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

		// Note: (power_saving): same as low_latency - use GetLoad().
		if (preferMin && core != NULL && core->GetLoad() > kHighLoad)
			core = NULL;

		// For P-cores, respect the 80% raw-load ceiling.
		if (preferMax && core != NULL && core->GetLoad() > 800)
			core = NULL;

		// 3-type intermediate fallback: P overloaded → try STANDARD before
		// giving up type preference and falling to the general packing search.
		if (preferMax && core == NULL && gHasStandardCores) {
			int32 stdBestScore = -1;
			bool foundNonOverloadedStd = false;
			bool tryRandomStd = gPackageCount > kRandomSearchThreshold;

			if (tryRandomStd && !useMask) {
				search_global_random(PackagePackingAction(
					cpu, NULL, core, stdBestScore, foundNonOverloadedStd,
					CORE_TYPE_STANDARD));
			} else if (useMask) {
				check_masked_packages_packing(cpu, mask, core, stdBestScore,
											  foundNonOverloadedStd,
											  CORE_TYPE_STANDARD);
			}

			if (core == NULL && !useMask) {
				int32 startIndex =
					tryRandomStd
						? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >>
								  32)
						: 0;
				int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);
				for (int32 i = 0; i < attempts; i++) {
					int32 index = startIndex + i;
					if (index >= gPackageCount)
						index -= gPackageCount;
					check_package_packing(
						cpu, &gPackageEntries[index], NULL, core, stdBestScore,
						foundNonOverloadedStd, CORE_TYPE_STANDARD);
				}
			}
		}

		if (core != NULL)
			return core;
	}

	// try to pack all threads on one core
	core = choose_small_task_core(cpu);
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
			if (previousCore != NULL && !has_cache_expired(threadData, now)) {
				PackageEntry* package = previousCore->Package();
				if (package != NULL) {
					CheckPackageMinimumLoad(cpu, package, NULL, bestCore,
											bestScore);
				}
			}

			// Phase 2: Local Node
			SchedulerNode* node = NULL;
			if (previousCore != NULL)
				node = previousCore->Package()->Node();
			else if (threadData->HomePackage() >= 0 &&
					 threadData->HomePackage() < gPackageCount) {
				node = gPackageEntries[threadData->HomePackage()].Node();
			}

			MinimumLoadAction minLoadAction(cpu, NULL, bestCore, bestScore);
			search_local_node(node, minLoadAction);

			// Phase 3: Global Random
			search_global_random(minLoadAction);

		} else if (useMask) {
			CheckMaskedPackagesMinimumLoad(cpu, mask, bestCore, bestScore);
		}

		// Fallback to full scan
		if (bestCore == NULL && !useMask) {
			int32 startIndex =
				tryRandom
					? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
					: 0;
			int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

			// Note: (power_saving).
			for (int32 i = 0; i < attempts; i++) {
				int32 index = startIndex + i;
				if (index >= gPackageCount)
					index -= gPackageCount;
				if (CheckPackageMinimumLoad(cpu, &gPackageEntries[index], NULL,
											bestCore, bestScore))
					break;
			}
		}

		core = bestCore;

		if (core == NULL) {
			core = choose_idle_core(cpu, useMask ? &mask : NULL);
			// also enforce thread-coloring type preference on
			// the idle-core result. choose_idle_core() ignores both affinity
			// masks and core-type constraints; without this guard a background
			// thread (preferMin) could receive a P-core, or a high-priority
			// foreground thread (preferMax) an E-core, defeating the coloring
			// decisions made higher up.
			if (core != NULL) {
				if (useMask && !core->CPUMask().Matches(mask))
					core = NULL;
				else if (preferMax && core->Type() != gMaxCoreType &&
						 gMinCoreType != gMaxCoreType)
					core = NULL;
				else if (preferMin && core->Type() != gMinCoreType &&
						 gMinCoreType != gMaxCoreType)
					core = NULL;
			}
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
					int bit = scheduler_ctz((native_cpu_mask_t)bits);
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
			// Note: the inner loop may exit with a non-NULL core
			// that still fails CPUMask().Matches() when the final for-loop
			// iteration found a core but the Matches() guard was false, causing
			// the outer for-loop to continue and eventually expire with a core
			// that does not satisfy the affinity constraint.  Null it out so
			// the NULL-return path below handles it cleanly rather than
			// returning a mismatched core to ChooseCoreAndCPU.
			if (core != NULL && !core->CPUMask().Matches(mask))
				core = NULL;
		}
	}

	if (core == NULL)
		return NULL;

	ASSERT(core != NULL);
	return core;
}

static CoreEntry* rebalance(const ThreadData* threadData, const CPUSet& mask,
							bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (now == 0)
		now = system_time();

	ASSERT(!gSingleCore);

	// Real-time threads bypass rebalancing to ensure zero jitter
	if (threadData->IsRealTime())
		return threadData->Core();

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
	const bool useMask = !mask.IsEmpty();

	// Returning NULL from rebalance signals "force full search" to
	// ChooseCoreAndCPU, which eventually calls MigrateTo().  If every core is
	// disabled, MigrateTo(NULL) leaves fCore == NULL and Enqueue faults.
	// Return the current core instead; ChooseCoreAndCPU will re-evaluate.
	CoreEntry* core = threadData->Core();
	if (core == NULL)
		return NULL;

	// Note: add belt-and-suspenders NULL guards for Package() and
	// Node() throughout rebalance. The top-level core != NULL check is
	// necessary but insufficient: Package() can return NULL for a core whose
	// Init() was skipped (exceeded kMaxCoresPerPackage), and Node() can
	// return NULL during topology teardown. Guard all dereference sites.
	if (core->Package() == NULL) {
		// Core exists but has no package (partially initialised or being
		// disabled). Return the current core - no rebalancing possible.
		return core;
	}

	int32 coreScore = core->GetScore();
	// Note: GetLoad() returns the thread's individual CPU load
	// contribution, not the total core load. Dividing again by cpuCount
	// produces a value cpuCount times too small, effectively disabling
	// migration on SMT systems. Use GetLoad() directly.
	int32 threadLoad = threadData->GetLoad();
	if (coreScore > kHighLoad) {
		int32 nodeID = -1;
		if (core->Package()->Node() != NULL)
			nodeID = core->Package()->Node()->ID();
		// If Node() is NULL (topology teardown), nodeID stays -1 and the
		// sSmallTaskCore branch below is skipped safely.

		if (nodeID >= 0 && nodeID < gNodeCount && sSmallTaskCore != NULL &&
			atomic_pointer_get<CoreEntry>(&sSmallTaskCore[nodeID]) == core) {
			atomic_pointer_set<CoreEntry>(&sSmallTaskCore[nodeID],
										  (CoreEntry*)NULL);
			CoreEntry* smallTaskCore = choose_small_task_core(cpu);

			if (threadLoad > coreScore / 3 || smallTaskCore == NULL ||
				(useMask && !smallTaskCore->CPUMask().Matches(mask))) {
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
			SchedulerNode* node = NULL;
			if (core->Package() != NULL)
				node = core->Package()->Node();

			PackagePackingAction rebalanceAction(cpu, NULL, other, bestScore,
												 foundNonOverloaded);
			search_local_node(node, rebalanceAction);

			// Phase 3: Global Random
			search_global_random(rebalanceAction);

		} else if (useMask) {
			check_masked_packages_packing(cpu, mask, other, bestScore,
										  foundNonOverloaded);
		}

		if (other == NULL && !useMask) {
			// Phase 4: Limited Global Scan (Fallback)
			int32 startIndex =
				tryRandom
					? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
					: 0;
			int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

			for (int32 i = 0; i < attempts; i++) {
				int32 index = startIndex + i;
				if (index >= gPackageCount)
					index -= gPackageCount;
				check_package_packing(cpu, &gPackageEntries[index], NULL, other,
									  bestScore, foundNonOverloaded);
			}
		}

		// If other is NULL, we failed to find candidate.
		if (other == NULL) {
			if (core->CPUCount() == 0)
				return NULL;  // Force migration to *any* core by triggering
							  // full search
			return core;	  // Fallback
		}
		ASSERT(other != NULL);

		int32 threshold = kLoadDifference >> 1;

		// Advanced NUMA Support:
		// If the candidate core 'other' is in the thread's Home Package,
		// we reduce the migration threshold to encourage returning home.
		// Conversely, if 'other' is remote and we are currently home, we
		// increase it.
		int32 homePackageID = threadData->HomePackage();
		if (homePackageID >= 0 && core->Package() != NULL &&
			other->Package() != NULL) {
			int32 currentPackageID = core->Package()->ID();
			int32 otherPackageID = other->Package()->ID();

			if (otherPackageID == homePackageID &&
				currentPackageID != homePackageID) {
				// Bonus for returning home: effectively 0 threshold
				threshold = 0;
			} else if (currentPackageID == homePackageID &&
					   otherPackageID != homePackageID) {
				// Penalty for leaving home: double the threshold.
				threshold *= 2;
			}
		}

		// Type-affinity guard: resist cross-type migration on heterogeneous
		// systems to preserve the placement made by choose_core. Without this,
		// a high-priority thread on a P-core is immediately rebalanced to a
		// lightly loaded E-core (which has a lower normalized score), defeating
		// thread coloring on every scheduling quantum.
		if (gMinCoreType != gMaxCoreType) {
			bool isFgRebal = threadData->IsForeground();
			int32 prioRebal = threadData->GetPriority();
			CoreType wantedType =
				(prioRebal > B_DISPLAY_PRIORITY || isFgRebal) ? gMaxCoreType
				: (prioRebal < B_NORMAL_PRIORITY && !isFgRebal)
					? gMinCoreType
					: CORE_TYPE_UNKNOWN;

			if (wantedType != CORE_TYPE_UNKNOWN && core->Type() == wantedType &&
				other->Type() != wantedType) {
				// Require double the normal load difference before accepting a
				// cross-type migration. A severely overloaded P-core can still
				// shed threads; a mildly overloaded one cannot.
				threshold += kLoadDifference;
			}
		}

		int32 weightedLoadOnCore =
			((int64)threadLoad * core->ScoreFactor()) >> 16;
		int32 weightedLoadOnOther =
			((int64)threadLoad * other->ScoreFactor()) >> 16;

		int32 coreNewScore = coreScore - weightedLoadOnCore;
		int32 otherNewScore = other->GetScore() + weightedLoadOnOther;

		if (coreNewScore - otherNewScore < threshold)
			return core;

		if (now - threadData->GetThread()->lastMigrationTime <
			kIdleConsolidationCooldown)
			return core;

		threadData->GetThread()->lastMigrationTime = now;
		return other;
	}

	if (coreScore >= kMediumLoad)
		return core;

	// Package() and Node() can return NULL during topology
	// teardown or if this core was never fully initialised.  Guard all
	// three pointer dereferences before accessing nodeID.
	if (core->Package() == NULL || core->Package()->Node() == NULL)
		return core;

	int32 nodeID = core->Package()->Node()->ID();
	if (nodeID < 0 || nodeID >= gNodeCount)
		return core;

	CoreEntry* smallTaskCore = choose_small_task_core(cpu);
	if (smallTaskCore == NULL ||
		(useMask && !smallTaskCore->CPUMask().Matches(mask)))
		return core;
	return smallTaskCore->GetScore() + threadLoad < kHighLoad ? smallTaskCore
															  : core;
}

static void rebalance_irqs(bool idle) {
	SCHEDULER_ENTER_FUNCTION();

	bool pack = false;
	bool hasSmallTaskCore = false;
	if (sSmallTaskCore != NULL) {
		for (int32 i = 0; i < gNodeCount; i++) {
			if (atomic_pointer_get<CoreEntry>(&sSmallTaskCore[i]) != NULL) {
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

	// Note: CoreEntry::GetCore() can return NULL if the CPU was just
	// hot-unplugged. Guard all dereferences of currentCore below.
	if (currentCore == NULL)
		return;

	// Package() and Node() can be NULL during topology teardown or if a core
	// was never fully initialised (e.g. it exceeded kMaxCoresPerPackage and
	// its Init() was skipped).  Defend all three pointer dereferences.
	if (pack && sSmallTaskCore != NULL && currentCore != NULL &&
		currentCore->Package() != NULL &&
		currentCore->Package()->Node() != NULL) {
		int32 nodeID = currentCore->Package()->Node()->ID();
		if (nodeID >= 0 && nodeID < gNodeCount &&
			atomic_pointer_get<CoreEntry>(&sSmallTaskCore[nodeID]) ==
				currentCore) {
			return;
		}
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
	int32 snapTotalLoad = totalLoad;

	locker.Unlock();

	// Note: document that snapTotalLoad uses the snapshotted value.
	// The original code re-read totalLoad after the unlock, creating a TOCTOU
	// window where a concurrent IRQ assignment change could cause kLowLoad
	// check to use a different value than chosenIRQ selection. The snapshot
	// (snapTotalLoad) correctly reflects the state at IRQ selection time.

	// Note: use snapshotted totalLoad and check chosenIRQ.
	if (chosenIRQ == -1 || (!pack && snapTotalLoad < kLowLoad))
		return;

	CoreEntry* other = NULL;
	if (pack) {
		if (sSmallTaskCore != NULL && currentCore != NULL &&
			currentCore->Package() != NULL &&
			currentCore->Package()->Node() != NULL) {
			int32 nodeID = currentCore->Package()->Node()->ID();
			if (nodeID >= 0 && nodeID < gNodeCount)
				other = (CoreEntry*)atomic_pointer_get<CoreEntry>(
					&sSmallTaskCore[nodeID]);
		}
	} else {
		int32 bestScore = -2;

		// Use random sampling if possible
		bool tryRandom = gPackageCount > kRandomSearchThreshold;

		CPUEntry* cpuEntryForIRQ = CPUEntry::GetCPU(cpu->cpu_num);

		if (tryRandom) {
			// Phase 2: Local Node
			// Do NOT re-read CoreEntry::GetCore() here.  currentCore
			// was snapshotted at function entry.  Re-reading it after the
			// SpinLocker unlock creates a TOCTOU window: a concurrent CPU
			// hot-unplug can change the assignment between the two reads,
			// producing an inconsistent view of Package() and Node() that
			// can lead to a NULL dereference or stale-pointer access.
			MinimumLoadAction irqMinLoadAction(cpuEntryForIRQ, NULL, other,
											   bestScore);
			if (currentCore != NULL && currentCore->Package() != NULL) {
				SchedulerNode* node = currentCore->Package()->Node();
				if (node != NULL) {
					search_local_node(node, irqMinLoadAction);
				}
			}

			// Phase 3: Global Random
			search_global_random(irqMinLoadAction);
		}

		if (other == NULL) {
			// Limit fallback attempts
			int32 startIndex =
				tryRandom ? (int32)(((uint64)cpuEntryForIRQ->GetRandom() *
									 gPackageCount) >>
									32)
						  : 0;
			int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

			// Note: (power_saving).
			for (int32 i = 0; i < attempts; i++) {
				int32 index = startIndex + i;
				if (index >= gPackageCount)
					index -= gPackageCount;
				if (CheckPackageMinimumLoad(cpuEntryForIRQ,
											&gPackageEntries[index], NULL,
											other, bestScore))
					break;
			}
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
	if (!pack && other->GetScore() + kLoadDifference >= currentCore->GetScore())
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
	{3, 10},

	20000,

	switch_to_mode,
	set_cpu_enabled,
	has_cache_expired,
	NULL,
	choose_core,
	rebalance,
	rebalance_irqs,
};

}  // namespace Scheduler
