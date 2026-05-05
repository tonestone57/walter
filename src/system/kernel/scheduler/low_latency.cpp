// AUDIT FIX: issue 8
/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 * Audit fixes applied 2025.
 */


#include <util/AutoLock.h>
#include <util/Random.h>

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"
#include "scheduler_topology.h"


namespace Scheduler {

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
		return CheckPackageMinimumLoad(cpu, entry, mask, bestCore, bestLoad, type);
	}
};




static const int kMigrationThreshold = 2;
static const bigtime_t kMigrationCooldown = 1000;
static const int kMaxCPUsToScan = 8;


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
	CoreEntry* core = threadData->PreviousCore();
	if (core == NULL)
		return true;
	bigtime_t activeTime = core->GetActiveTime();
	return activeTime - threadData->WentSleepActive() > kCacheExpire;
}








static CoreEntry*
choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	CPUSet mask = threadData->GetCPUMask();
	bool useMask = !mask.IsEmpty();
	if (useMask && Scheduler::IsAllEnabledMask(mask))
		useMask = false;

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
	CoreEntry* previousCore = threadData->PreviousCore();

	const bool cacheExpired = (previousCore != NULL)
		? has_cache_expired(threadData) : true;

	if (previousCore != NULL && previousCore->GetScore() == 0) {
		CoreEntry* currentCore = cpu->Core();
		if (currentCore != NULL
				&& previousCore->Package() == currentCore->Package()
				&& (!useMask || previousCore->CPUMask().Matches(mask)))
			return previousCore;
	}

	bool isForeground = threadData->IsForeground();
	int32 priority = threadData->GetPriority();
	bool preferMax = (priority > B_DISPLAY_PRIORITY || isForeground)
		&& (gMinCoreType != gMaxCoreType);
	bool preferMin = (priority < B_NORMAL_PRIORITY && !isForeground)
		&& (gMinCoreType != gMaxCoreType);

	if (previousCore != NULL && !cacheExpired) {
		if (!useMask || previousCore->CPUMask().Matches(mask)) {
			bool typeMatch = true;
			if (preferMax && previousCore->Type() != gMaxCoreType)
				typeMatch = false;
			else if (preferMin && previousCore->Type() != gMinCoreType)
				typeMatch = false;

			if (typeMatch) {
				if (previousCore->GetLoad() == 0)
					return previousCore;

				if (previousCore->GetScore() <= kLoadDifference)
					return previousCore;
			}
		}

		PackageEntry* package = previousCore->Package();
		if (package != NULL) {
			CoreEntry* sibling = package->GetIdleCore();
			if (sibling != NULL && (!useMask || sibling->CPUMask().Matches(mask)))
				return sibling;

			SchedulerNode* node = package->Node();
			if (node != NULL) {
				uint64 idlePackageMask = node->IdlePackageMask();
				while (idlePackageMask != 0) {
					int32 packageIndex = scheduler_ffs64(idlePackageMask) - 1;
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

	uint64 idleNodeMask = atomic_get64((int64*)&gIdleNodeMask);

	if (preferMax || preferMin) {
		CoreType preferredType = preferMax ? gMaxCoreType : gMinCoreType;
		int32 bestScore = -1;
		MinimumLoadAction minLoadAction(cpu, NULL, core, bestScore, preferredType);

		uint64 typeIdleNodeMask = idleNodeMask;
		while (typeIdleNodeMask != 0) {
			int32 nodeIndex = scheduler_ffs64(typeIdleNodeMask) - 1;
			typeIdleNodeMask &= ~(1ULL << nodeIndex);

			SchedulerNode* node = &gSchedulerNodes[nodeIndex];
			uint64 idlePackageMask = node->IdlePackageMask();

			while (idlePackageMask != 0) {
				int32 packageIndex = scheduler_ffs64(idlePackageMask) - 1;
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
			bestScore = -1;
			bool tryRandom = gPackageCount > kRandomSearchThreshold;
			if (tryRandom && !useMask) {
				search_global_random(MinimumLoadAction(cpu, NULL, core,
					bestScore, preferredType));
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

			if (preferMin && core != NULL && core->GetLoad() > kHighLoad)
				core = NULL;

			if (preferMax && core != NULL && core->GetLoad() > 800)
				core = NULL;
		}

		if (core != NULL) {
			if (!preferMax || core->GetScore() < kMediumLoad) {
				if (core->GetLoad() == 0) {
					return core;
				}
			}
		}

		if (preferMax && gHasStandardCores) {
			int32 stdBestScore = -1;
			bool tryRandomStd = gPackageCount > kRandomSearchThreshold;

			if (tryRandomStd && !useMask) {
				search_global_random(MinimumLoadAction(cpu, NULL, core,
					stdBestScore, CORE_TYPE_STANDARD));
			} else if (useMask) {
				CheckMaskedPackagesMinimumLoad(cpu, mask, core, stdBestScore,
					CORE_TYPE_STANDARD);
			}

			if (core == NULL && !useMask) {
				int32 startIndex2 = tryRandomStd
					? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
					: 0;
				int32 attemptsFallback = min_c(gPackageCount, kMaxFallbackAttempts);
				for (int32 i = 0; i < attemptsFallback; i++) {
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

	bool skipIdleScan = ((preferMax || preferMin) && core != NULL);

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
			CoreEntry* bestHomeCore = NULL;
			int32 bestHomeLoad = -1;
			bool nearIdle = CheckPackageMinimumLoad(cpu, homePackage,
				useMask ? &mask : NULL, bestHomeCore, bestHomeLoad,
				preferredType);

			if (nearIdle || (bestHomeCore != NULL
					&& bestHomeLoad < kLoadDifference))
				core = bestHomeCore;
		}
	}

	int scannedCount = 0;
	while (!skipIdleScan && idleNodeMask != 0) {
		if (++scannedCount > kMaxCPUsToScan)
			break;

		int32 nodeIndex = scheduler_ffs64(idleNodeMask) - 1;
		idleNodeMask &= ~(1ULL << nodeIndex);

		SchedulerNode* node = &gSchedulerNodes[nodeIndex];
		uint64 idlePackageMask = node->IdlePackageMask();

		while (idlePackageMask != 0) {
			int32 packageIndex = scheduler_ffs64(idlePackageMask) - 1;
			idlePackageMask &= ~(1ULL << packageIndex);

			int32 globalPackageIndex
				= node->PackageStartIndex() + packageIndex;
			if (globalPackageIndex >= gPackageCount)
				continue;

			PackageEntry* package = &gPackageEntries[globalPackageIndex];
			native_cpu_mask_t idleMask = package->IdleCoreMask();
			while (idleMask != 0) {
				int32 bitIdx = scheduler_ctz(idleMask);
				idleMask &= ~((native_cpu_mask_t)1 << bitIdx);

				CoreEntry* candidate = package->GetCore(bitIdx);
				if (candidate != NULL
						&& (!useMask || candidate->CPUMask().Matches(mask))) {
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
		CoreEntry* bestCore = NULL;
		int32 bestLoad = -1;
		MinimumLoadAction globalMinLoadAction(cpu, NULL, bestCore, bestLoad);

		bool tryRandom = gPackageCount > kRandomSearchThreshold;

		if (tryRandom && !useMask) {
			SchedulerNode* node = NULL;
			if (previousCore != NULL && previousCore->Package() != NULL)
				node = previousCore->Package()->Node();
			else if (homePackageID >= 0 && homePackageID < gPackageCount)
				node = gPackageEntries[homePackageID].Node();

			search_local_node(node, globalMinLoadAction);

			search_global_random(globalMinLoadAction);

		} else if (useMask) {
			CheckMaskedPackagesMinimumLoad(cpu, mask, bestCore, bestLoad);
		}

		if (bestCore == NULL && !useMask) {
			int32 startIndex = tryRandom
				? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
				: 0;
			int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

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

			if (core != NULL && !core->CPUMask().Matches(mask))
				core = NULL;
		}
	}

	ASSERT(core != NULL);

	if (previousCore != NULL && !cacheExpired) {
		if (!useMask || previousCore->CPUMask().Matches(mask)) {
			if (core != previousCore) {
				bool typeOk = true;
				if (preferMax && previousCore->Type() != gMaxCoreType)
					typeOk = false;
				else if (preferMin && previousCore->Type() != gMinCoreType)
					typeOk = false;

				if (core == NULL)
					return previousCore;

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

	if (threadData->IsRealTime())
		return threadData->Core();

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
	CoreEntry* core = threadData->Core();
	ASSERT(core != NULL);

	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	CoreEntry* other = NULL;
	int32 bestLoad = -1;

	bool tryRandom = gPackageCount > kRandomSearchThreshold;

	if (tryRandom && !useMask) {
		SchedulerNode* node = NULL;
		if (core->Package() != NULL)
			node = core->Package()->Node();

		MinimumLoadAction rebalanceMinLoadAction(cpu, NULL, other, bestLoad);
		search_local_node(node, rebalanceMinLoadAction);

		search_global_random(rebalanceMinLoadAction);

	} else if (useMask) {
		CheckMaskedPackagesMinimumLoad(cpu, mask, other, bestLoad);
	}

	if (other == NULL && !useMask) {
		int32 startIndex = tryRandom
			? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
			: 0;
		int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

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
			return NULL;
		return core;
	}
	ASSERT(other != NULL);

	int32 otherScore = other->GetScore();
	int32 coreScore = core->GetScore();

	if (other == core)
		return core;

	bigtime_t coreVRuntime = core->GetMinVirtualRuntime();
	bigtime_t otherVRuntime = other->GetMinVirtualRuntime();

	bool congested = (coreVRuntime >= (bigtime_t)(INT64_MIN + 20000LL))
		&& (coreVRuntime <= (bigtime_t)(INT64_MAX - 20000LL))
		&& (otherVRuntime > coreVRuntime + 20000LL);

	int32 threshold = (kLoadDifference * core->PerformanceScale()) >> kDefaultCapacityShift;
	if (congested)
		threshold = 0;

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
			threshold = max_c(threshold, kLoadDifference * 2);
		}
	}

	int32 homePackageID = threadData->HomePackage();
	if (homePackageID >= 0 && core->Package() != NULL && other->Package() != NULL) {
		int32 currentPackageID = core->Package()->ID();
		int32 otherPackageID = other->Package()->ID();

		if (otherPackageID == homePackageID && currentPackageID != homePackageID) {
			threshold = 0;
		} else if (currentPackageID == homePackageID && otherPackageID != homePackageID) {
			threshold *= 2;
		}
	}

	if (otherScore + threshold >= coreScore)
		return core;

	int32 difference = coreScore - otherScore - threshold;
	ASSERT(difference > 0);

	int32 threadLoad = threadData->GetLoad();

	int32 weightedLoadOnCore = ((int64)threadLoad * core->ScoreFactor()) >> 16;
	int32 weightedLoadOnOther = ((int64)threadLoad * other->ScoreFactor()) >> 16;

	int32 coreNewScore = coreScore - weightedLoadOnCore;
	int32 otherNewScore = otherScore + weightedLoadOnOther;

	if (coreNewScore - otherNewScore < threshold)
		return core;

	bigtime_t now = system_time();
	if (now - threadData->GetThread()->lastMigrationTime < kMigrationCooldown)
		return core;

	threadData->GetThread()->lastMigrationTime = now;
	return other;
}


static void
rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();

	if (idle)
		return;

	cpu_ent* cpu = get_cpu_struct();

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
	int32 snapTotalLoad = totalLoad;

	locker.Unlock();

	if (chosenIRQ == -1 || snapTotalLoad < kLowLoad)
		return;

	CoreEntry* other = NULL;
	int32 bestLoad = -1;

	bool tryRandom = gPackageCount > kRandomSearchThreshold;

	CPUEntry* cpuEntryForIRQ = CPUEntry::GetCPU(cpu->cpu_num);

	if (tryRandom) {
		MinimumLoadAction irqMinLoadAction(cpuEntryForIRQ, NULL, other, bestLoad);
		if (currentCore != NULL && currentCore->Package() != NULL) {
			SchedulerNode* node = currentCore->Package()->Node();
			search_local_node(node, irqMinLoadAction);
		}

		search_global_random(irqMinLoadAction);
	}

	if (other == NULL) {
		int32 startIndex = tryRandom
			? (int32)(((uint64)cpuEntryForIRQ->GetRandom() * gPackageCount) >> 32)
			: 0;
		int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

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

}	// namespace Scheduler
