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


using namespace Scheduler;


const bigtime_t kCacheExpire = 100000;

static const int32 kRandomSearchThreshold = 32;
static const int32 kRandomSamples = 16;


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


static void
check_package(PackageEntry* entry, const CPUSet* mask,
	CoreEntry*& bestCore, int32& bestLoad)
{
	entry->ReadLockCore();

	CoreEntry* candidate = entry->PeekMinimumLoadCore();

	if (candidate != NULL && (mask == NULL || candidate->CPUMask().Matches(*mask))) {
		int32 load = candidate->GetLoad();
		if (bestCore == NULL || load < bestLoad) {
			bestCore = candidate;
			bestLoad = load;
		}
	}
	entry->ReadUnlockCore();
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

			// Check local NUMA node (Super Package) to keep traffic local
			// This reduces interconnect saturation on large multi-socket systems.
			SchedulerNode* node = package->Node();
			if (node != NULL) {
				uint64 idlePackageMask = node->IdlePackageMask();
				while (idlePackageMask != 0) {
					int32 packageIndex = __builtin_ctzll(idlePackageMask);
					idlePackageMask &= ~(1ULL << packageIndex);

					int32 globalPackageIndex = node->NodeIndex() * 64 + packageIndex;
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

			int32 globalPackageIndex = nodeIndex * 64 + packageIndex;
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
			int32 visited[kRandomSamples];
			int32 samplesTaken = 0;
			int32 attempts = 0;
			const int32 kMaxAttempts = kRandomSamples * 2;

			while (samplesTaken < kRandomSamples && attempts++ < kMaxAttempts) {
				int32 i = fast_get_random<uint32>() % gPackageCount;

				// Avoid checking the same package twice
				bool collision = false;
				for (int32 j = 0; j < samplesTaken; j++) {
					if (visited[j] == i) {
						collision = true;
						break;
					}
				}
				if (collision)
					continue;
				visited[samplesTaken++] = i;

				check_package(&gPackageEntries[i], NULL, bestCore, bestLoad);
			}
		} else if (useMask) {
			// Iterate over allowed CPUs to find candidate packages
			const int32 kCPUSetArraySize = (SMP_MAX_CPUS + 31) / 32;
			const int32 cpuCount = smp_get_num_cpus();
			PackageEntry* lastPackage = NULL;

			for (int32 i = 0; i < kCPUSetArraySize; i++) {
				uint32 bits = mask.Bits(i);
				while (bits != 0) {
					int bit = __builtin_ctz(bits);
					bits &= ~(1U << bit);
					int32 cpuID = i * 32 + bit;

					if (cpuID >= cpuCount)
						continue;

					// We need to find the package for this CPU.
					CoreEntry* cpuCore = CPUEntry::GetCPU(cpuID)->Core();
					if (cpuCore != NULL) {
						PackageEntry* package = cpuCore->Package();
						if (package != NULL && package != lastPackage) {
							check_package(package, &mask, bestCore, bestLoad);
							lastPackage = package;
						}
					}
				}
			}
		}

		// Fallback to full scan ONLY if we are not using random sampling (small system)
		// AND we didn't use mask iteration (handled above).
		// OR if random sampling failed completely to find *any* candidate (unlikely unless all broken).
		if (bestCore == NULL && !tryRandom && !useMask) {
			for (int32 i = 0; i < gPackageCount; i++) {
				check_package(&gPackageEntries[i], NULL, bestCore, bestLoad);
			}
		}

		// If we still haven't found a core (e.g. random sampling failed to find any valid core,
		// or mask iteration found nothing - which shouldn't happen if mask is valid),
		// we might need a desperate fallback.
		// However, for random sampling, we should have found something.
		if (bestCore == NULL && tryRandom && !useMask) {
			// Extremely unlikely: random sampling yielded no results (e.g. all sampled packages empty/disabled?)
			// Fallback to scanning everything.
			for (int32 i = 0; i < gPackageCount; i++) {
				check_package(&gPackageEntries[i], NULL, bestCore, bestLoad);
			}
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
	int32 bestLoad = -1;

	// Use random sampling if possible
	bool tryRandom = gPackageCount > kRandomSearchThreshold;

	if (tryRandom && !useMask) {
		int32 visited[kRandomSamples];
		int32 samplesTaken = 0;
		int32 attempts = 0;
		const int32 kMaxAttempts = kRandomSamples * 2;

		while (samplesTaken < kRandomSamples && attempts++ < kMaxAttempts) {
			int32 i = fast_get_random<uint32>() % gPackageCount;

			// Avoid checking the same package twice
			bool collision = false;
			for (int32 j = 0; j < samplesTaken; j++) {
				if (visited[j] == i) {
					collision = true;
					break;
				}
			}
			if (collision)
				continue;
			visited[samplesTaken++] = i;

			check_package(&gPackageEntries[i], NULL, other, bestLoad);
		}
	} else if (useMask) {
		const int32 kCPUSetArraySize = (SMP_MAX_CPUS + 31) / 32;
		const int32 cpuCount = smp_get_num_cpus();
		PackageEntry* lastPackage = NULL;

		for (int32 i = 0; i < kCPUSetArraySize; i++) {
			uint32 bits = mask.Bits(i);
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
						check_package(package, &mask, other, bestLoad);
						lastPackage = package;
					}
				}
			}
		}
	}

	if (other == NULL && !tryRandom && !useMask) {
		for (int32 i = 0; i < gPackageCount; i++) {
			check_package(&gPackageEntries[i], NULL, other, bestLoad);
		}
	}

	if (other == NULL && tryRandom && !useMask) {
		// Fallback for random failure
		for (int32 i = 0; i < gPackageCount; i++) {
			check_package(&gPackageEntries[i], NULL, other, bestLoad);
		}
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
	int32 bestLoad = -1;

	// Use random sampling if possible
	bool tryRandom = gPackageCount > kRandomSearchThreshold;

	if (tryRandom) {
		int32 visited[kRandomSamples];
		int32 samplesTaken = 0;
		int32 attempts = 0;
		const int32 kMaxAttempts = kRandomSamples * 2;

		while (samplesTaken < kRandomSamples && attempts++ < kMaxAttempts) {
			int32 i = fast_get_random<uint32>() % gPackageCount;

			// Avoid checking the same package twice
			bool collision = false;
			for (int32 j = 0; j < samplesTaken; j++) {
				if (visited[j] == i) {
					collision = true;
					break;
				}
			}
			if (collision)
				continue;
			visited[samplesTaken++] = i;

			check_package(&gPackageEntries[i], NULL, other, bestLoad);
		}
	}

	// Use empty mask (NULL), as we don't care about affinity here
	if (other == NULL && !tryRandom) {
		for (int32 i = 0; i < gPackageCount; i++) {
			check_package(&gPackageEntries[i], NULL, other, bestLoad);
		}
	}

	if (other == NULL && tryRandom) {
		// Fallback for random failure
		for (int32 i = 0; i < gPackageCount; i++) {
			check_package(&gPackageEntries[i], NULL, other, bestLoad);
		}
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
