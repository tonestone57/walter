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


const bigtime_t kCacheExpire = 100000;

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
	if (threadData->WentSleep() == 0)
		return false;
	return system_time() - threadData->WentSleep() > kCacheExpire;
}


static void
check_package_small_task(PackageEntry* entry, CoreEntry*& core, int32& bestLoad)
{
	entry->ReadLockCore();

	// Find the core with highest load that isn't overloaded.
	// If all are overloaded, we might pick the least loaded later.
	// For choosing a small task core, we want packing.
	CoreEntry* candidate = entry->PeekMaximumLoadCore();

	if (candidate != NULL) {
		int32 load = candidate->GetLoad();
		if (core == NULL || load > bestLoad) {
			core = candidate;
			bestLoad = load;
		}
	}
	entry->ReadUnlockCore();
}


static CoreEntry*
choose_small_task_core()
{
	SCHEDULER_ENTER_FUNCTION();

	CoreEntry* core = NULL;
	int32 bestLoad = -1;

	for (int32 i = 0; i < gPackageCount; i++) {
		check_package_small_task(&gPackageEntries[i], core, bestLoad);
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
		uint64 idlePackageMask = atomic_get64((int64*)&gIdlePackageMask);
		if (idlePackageMask != 0) {
			int32 packageIndex = __builtin_ctzll(idlePackageMask);
			package = &gPackageEntries[packageIndex];
		}
	}

	if (package != NULL)
		return package->GetIdleCore();
	return NULL;
}


static void
check_package_min_load(PackageEntry* entry, const CPUSet* mask,
	CoreEntry*& bestCore, int32& bestLoad)
{
	entry->ReadLockCore();

	CoreEntry* candidate = entry->PeekMinimumLoadCore(mask);

	if (candidate != NULL) {
		int32 load = candidate->GetLoad();
		if (bestCore == NULL || load < bestLoad) {
			bestCore = candidate;
			bestLoad = load;
		}
	}
	entry->ReadUnlockCore();
}


static void
check_masked_packages_min_load(const CPUSet& mask, CoreEntry*& bestCore, int32& bestLoad)
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
					check_package_min_load(package, &mask, bestCore, bestLoad);
					lastPackage = package;
				}
			}
		}
	}
}


static void
check_package_packing(PackageEntry* entry, const CPUSet* mask,
	CoreEntry*& other, int32& bestLoad, bool& foundNonOverloaded)
{
	entry->ReadLockCore();

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
	entry->ReadUnlockCore();
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


template <typename Action>
static void
search_local_node(SchedulerNode* node, Action action)
{
	if (node == NULL)
		return;

	int32 nodeBaseIndex = node->PackageStartIndex();
	int32 packagesInNode = node->PackageCount();

	if (nodeBaseIndex >= gPackageCount || packagesInNode <= 0)
		return;

	const int kMaxLocalAttempts = 4;
	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
	for (int i = 0; i < kMaxLocalAttempts; i++) {
		// Multiplicative random mapping to avoid expensive modulo
		int32 index = nodeBaseIndex
			+ (int32)(((uint64)cpu->GetRandom() * packagesInNode) >> 32);
		action(&gPackageEntries[index]);
	}
}


template <typename Action>
static void
search_global_random(Action action)
{
	int32 samplesToTake = gRandomSamples;
	int32 samplesTaken = 0;
	int32 attempts = 0;
	const int32 kMaxAttempts = samplesToTake * 2;

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());

	// Bitmask for tracking visited packages to avoid collisions.
	// Use a smaller fixed buffer on the stack (128 bytes = 1024 packages)
	// which covers >99% of systems. For massive systems, we skip collision
	// detection for indices beyond 1024 to save stack space.
	const int32 kStackBitmaskSize = 1024;
	uint64 visitedBits[kStackBitmaskSize / 64];

	int32 packagesToCheck = min_c(gPackageCount, kStackBitmaskSize);
	int32 wordsToClear = (packagesToCheck + 63) / 64;
	memset(visitedBits, 0, wordsToClear * sizeof(uint64));

	while (samplesTaken < samplesToTake && attempts++ < kMaxAttempts) {
		// Multiplicative random mapping to avoid expensive modulo
		int32 i = (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32);

		// Avoid checking the same package twice using the bitmask
		int32 word = i / 64;
		int32 bit = i % 64;

		// Only check collision if within our stack bitmask range
		if (i < kStackBitmaskSize) {
			if ((visitedBits[word] & (1ULL << bit)) != 0)
				continue;
			visitedBits[word] |= (1ULL << bit);
		}

		samplesTaken++;
		action(&gPackageEntries[i]);
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
					check_package_min_load(package, NULL, bestCore, bestLoad);
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
				check_package_min_load(entry, NULL, bestCore, bestLoad);
				return false;
			});

			// Phase 3: Global Random
			search_global_random([&](PackageEntry* entry) {
				check_package_min_load(entry, NULL, bestCore, bestLoad);
				return false;
			});

		} else if (useMask) {
			check_masked_packages_min_load(mask, bestCore, bestLoad);
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
				check_package_min_load(&gPackageEntries[index], NULL, bestCore, bestLoad);
			}
		}

		core = bestCore;

		if (core == NULL) {
			core = choose_idle_core();
			if (useMask && !core->CPUMask().Matches(mask))
				core = NULL;
		}
	}

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

	int32 coreLoad = core->GetLoad();
	int32 threadLoad = threadData->GetLoad() / core->CPUCount();
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
		int32 bestLoad = -1;
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
		if (other == NULL) return core; // Fallback
		ASSERT(other != NULL);

		int32 coreNewLoad = coreLoad - threadLoad;
		int32 otherNewLoad = other->GetLoad() + threadLoad;
		return coreNewLoad - otherNewLoad >= kLoadDifference >> 1 ? other : core;
	}

	if (coreLoad >= kMediumLoad)
		return core;

	CoreEntry* smallTaskCore = choose_small_task_core();
	if (smallTaskCore == NULL || (useMask && !smallTaskCore->CPUMask().Matches(mask)))
		return core;
	return smallTaskCore->GetLoad() + threadLoad < kHighLoad
		? smallTaskCore : core;
}


static inline void
pack_irqs()
{
	SCHEDULER_ENTER_FUNCTION();

	CoreEntry* smallTaskCore = atomic_pointer_get(&sSmallTaskCore);
	if (smallTaskCore == NULL)
		return;

	cpu_ent* cpu = get_cpu_struct();
	if (smallTaskCore == CoreEntry::GetCore(cpu->cpu_num))
		return;

	SpinLocker locker(cpu->irqs_lock);
	while (true) {
		irq_assignment* irq = (irq_assignment*)list_get_first_item(&cpu->irqs);
		if (irq == NULL)
			break;

		int32 irqVector = irq->irq;
		locker.Unlock();

		CoreCPUHeapLocker _(smallTaskCore);
		int32 newCPU = smallTaskCore->CPUHeap()->PeekRoot()->ID();
		_.Unlock();

		if (newCPU != cpu->cpu_num) {
			assign_io_interrupt_to_cpu(irqVector, newCPU);
		} else {
			locker.Lock();
			break;
		}

		locker.Lock();
	}
}


static void
rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();

	if (idle && sSmallTaskCore != NULL) {
		pack_irqs();
		return;
	}

	if (idle || sSmallTaskCore != NULL)
		return;

	cpu_ent* cpu = get_cpu_struct();
	SpinLocker locker(cpu->irqs_lock);

	irq_assignment* chosen = NULL;
	irq_assignment* irq = (irq_assignment*)list_get_first_item(&cpu->irqs);

	while (irq != NULL) {
		if (chosen == NULL || chosen->load < irq->load)
			chosen = irq;
		irq = (irq_assignment*)list_get_next_item(&cpu->irqs, irq);
	}

	locker.Unlock();

	if (chosen == NULL || chosen->load < kLowLoad)
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
				check_package_min_load(entry, NULL, other, bestLoad);
				return false;
			});
		}

		// Phase 3: Global Random
		search_global_random([&](PackageEntry* entry) {
			check_package_min_load(entry, NULL, other, bestLoad);
			return false;
		});

	} else {
		// Limit fallback attempts
		const int32 kMaxFallbackAttempts = 64;
		int32 startIndex = 0; // No random here as tryRandom was false (small system)
		int32 attempts = min_c(gPackageCount, kMaxFallbackAttempts);

		for (int32 i = 0; i < attempts; i++) {
			check_package_min_load(&gPackageEntries[i], NULL, other, bestLoad);
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
	if (other->GetLoad() + kLoadDifference >= core->GetLoad())
		return;

	assign_io_interrupt_to_cpu(chosen->irq, newCPU);
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
