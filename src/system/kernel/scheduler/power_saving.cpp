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

	MinimumLoadAction(CPUEntry* c, const CPUSet* m, CoreEntry*& bc, int32& bl)
		: cpu(c), mask(m), bestCore(bc), bestLoad(bl) {}

	bool operator()(PackageEntry* entry) const {
		return CheckPackageMinimumLoad(cpu, entry, mask, bestCore, bestLoad);
	}
};

struct PackingAction {
	CPUEntry* cpu;
	const CPUSet* mask;
	CoreEntry*& bestCore;

	PackingAction(CPUEntry* c, const CPUSet* m, CoreEntry*& bc)
		: cpu(c), mask(m), bestCore(bc) {}

	bool operator()(PackageEntry* entry) const {
		bestCore = entry->GetIdleCorePacking(cpu, mask);
		return bestCore != NULL;
	}
};


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
	return activeTime - threadData->WentSleepActive() > kCacheExpire * 10;
}


static CoreEntry*
choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	// Core Selection Logic (Packing Strategy):
	// Prefers active cores to maximize power savings by keeping other cores idle.
	// Only wakes up new cores when load exceeds thresholds.

	CPUSet mask = threadData->GetCPUMask();
	bool useMask = !mask.IsEmpty();
	if (useMask && Scheduler::IsAllEnabledMask(mask))
		useMask = false;

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
	CoreEntry* previousCore = threadData->PreviousCore();

	if (previousCore != NULL && !has_cache_expired(threadData)) {
		if (!useMask || previousCore->CPUMask().Matches(mask)) {
			if (previousCore->GetLoad() < kHighLoad)
				return previousCore;
		}
	}

	CoreEntry* core = NULL;

	int32 bestLoad = -1;
	bool tryRandom = gPackageCount > kRandomSearchThreshold;

	if (tryRandom && !useMask) {
		SchedulerNode* node = NULL;
		if (previousCore != NULL && previousCore->Package() != NULL)
			node = previousCore->Package()->Node();
		search_local_node(node, MinimumLoadAction(cpu, NULL, core, bestLoad));
	} else if (useMask) {
		CheckMaskedPackagesMinimumLoad(cpu, mask, core, bestLoad);
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
			if (CheckPackageMinimumLoad(cpu, &gPackageEntries[index], NULL, core,
					bestLoad))
				break;
		}
	}

	if (core != NULL && core->GetLoad() < kHighLoad)
		return core;

	CoreEntry* idleCore = NULL;
	if (tryRandom && !useMask) {
		SchedulerNode* node = NULL;
		if (previousCore != NULL && previousCore->Package() != NULL)
			node = previousCore->Package()->Node();
		search_local_node(node, PackingAction(cpu, NULL, idleCore));
	} else if (useMask) {
		const int32 kCPUSetArraySize = (SMP_MAX_CPUS + 31) / 32;
		const int32 cpuCount = smp_get_num_cpus();
		for (int32 i = 0; i < kCPUSetArraySize; i++) {
			uint32 bits = mask.Bits(i);
			while (bits != 0) {
				int bit = scheduler_ctz((native_cpu_mask_t)bits);
				bits &= ~(1U << bit);
				int32 cpuID = i * 32 + bit;
				if (cpuID >= cpuCount) continue;
				CoreEntry* candidate = CPUEntry::GetCPU(cpuID)->Core();
				if (candidate != NULL && candidate->GetLoad() == 0) {
					idleCore = candidate;
					break;
				}
			}
			if (idleCore != NULL) break;
		}
	}

	if (idleCore == NULL && !useMask) {
		int32 startIndex = tryRandom
			? (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32)
			: 0;
		int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

		for (int32 i = 0; i < attempts; i++) {
			int32 index = startIndex + i;
			if (index >= gPackageCount)
				index -= gPackageCount;
			idleCore = gPackageEntries[index].GetIdleCorePacking(cpu);
			if (idleCore != NULL)
				break;
		}
	}

	if (idleCore != NULL)
		return idleCore;

	if (core != NULL)
		return core;

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
				if (cpuID >= cpuCount) continue;
				core = CPUEntry::GetCPU(cpuID)->Core();
				if (core != NULL) break;
			}
			if (core != NULL) break;
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

	if (other == NULL)
		return core;
	ASSERT(other != NULL);

	if (other == core)
		return core;

	int32 otherScore = other->GetScore();
	int32 coreScore = core->GetScore();

	if (coreScore <= kHighLoad)
		return core;

	if (otherScore + kLoadDifference >= coreScore)
		return core;

	return other;
}


static void
rebalance_irqs(bool /* idle */)
{
}


scheduler_mode_operations gSchedulerPowerSavingMode = {
	"power saving",

	3000,
	2000,
	{ 1, 3 },

	10000,

	switch_to_mode,
	set_cpu_enabled,
	has_cache_expired,
	choose_core,
	rebalance,
	rebalance_irqs,
};


}	// namespace Scheduler
