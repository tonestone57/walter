// AUDIT FIXES: issues 5, 17, 26, 51, 71, 79
/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 *
 * Audit and robustness fixes (2025).
 */


#ifndef KERNEL_SCHEDULER_TOPOLOGY_H
#define KERNEL_SCHEDULER_TOPOLOGY_H


#include <string.h>

#include <util/Random.h>

#include "scheduler_cpu.h"


namespace Scheduler {


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

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());

	if (packagesInNode <= 8) {
		int32 lastIndex = atomic_get(&cpu->fLastLocalPackageIndex);
		int32 start = (lastIndex + 1) % packagesInNode;
		for (int32 i = 0; i < packagesInNode; i++) {
			int32 offset = start + i;
			if (offset >= packagesInNode)
				offset -= packagesInNode;
			int32 index = nodeBaseIndex + offset;
			if (index >= gPackageCount)
				continue;
			atomic_set(&cpu->fLastLocalPackageIndex, offset);
			if (action(&gPackageEntries[index]))
				break;
		}
		return;
	}

	int32 logPackages = 0;
	if (packagesInNode > 1)
		logPackages = fls((uint32)packagesInNode) - 1;

	const int kMaxLocalAttempts = min_c(packagesInNode, 4 + logPackages);

	uint64 visitedLocal = 0;
	const bool canDedup = (packagesInNode <= 64);

	for (int i = 0; i < kMaxLocalAttempts; i++) {
		int32 index = nodeBaseIndex
			+ (int32)(((uint64)cpu->GetRandom() * packagesInNode) >> 32);
		if (index >= gPackageCount)
			continue;

		if (canDedup) {
			int32 localIdx = index - nodeBaseIndex;
			if (localIdx >= 0 && localIdx < 64) {
				uint64 bit = 1ULL << localIdx;
				if (visitedLocal & bit)
					continue;
				visitedLocal |= bit;
			}
		}

		if (action(&gPackageEntries[index]))
			break;
	}
}


template <typename Action>
static void
search_global_random(Action action)
{
	const int32 packageCount = gPackageCount;

	if (packageCount <= 0)
		return;


	ASSERT(packageCount <= 4096);

	int32 samplesToTake = min_c(gRandomSamples, packageCount);
	int32 samplesTaken = 0;
	int32 attempts = 0;
	const int32 kMaxAttempts = samplesToTake * 8;

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());

	if (packageCount <= 64) {
		uint64 visitedBits = 0;
		while (samplesTaken < samplesToTake && attempts++ < kMaxAttempts) {
			int32 i = (int32)(((uint64)cpu->GetRandom() * packageCount) >> 32);

			if ((visitedBits & (1ULL << i)) != 0)
				continue;
			visitedBits |= (1ULL << i);

			samplesTaken++;
			if (action(&gPackageEntries[i]))
				break;
		}
		return;
	}

	const int32 kStackBitmaskSize = 4096;
	uint64 visitedBits[kStackBitmaskSize / 64];

	int32 wordsNeeded = min_c((packageCount + 63) / 64,
		(int32)(kStackBitmaskSize / 64));
	memset(visitedBits, 0, (size_t)wordsNeeded * sizeof(uint64));

	while (samplesTaken < samplesToTake && attempts++ < kMaxAttempts) {
		int32 i = (int32)(((uint64)cpu->GetRandom() * packageCount) >> 32);

		int32 word = i / 64;
		int32 bit  = i % 64;

		if (i < kStackBitmaskSize) {
			if ((visitedBits[word] & (1ULL << bit)) != 0)
				continue;
			visitedBits[word] |= (1ULL << bit);
		}
		samplesTaken++;

		if (action(&gPackageEntries[i]))
			break;
	}
}


static inline bool
CheckPackageMinimumLoad(CPUEntry* cpu, PackageEntry* entry, const CPUSet* mask,
	CoreEntry*& bestCore, int32& bestLoad, CoreType type = CORE_TYPE_UNKNOWN)
{
	CoreEntry* candidate = entry->PeekMinimumLoadCore(cpu, mask, type);

	if (candidate != NULL) {
		int32 score = candidate->GetScore();

		const int32 kLowLoadThreshold = (kMaxLoad * 15) / 100;
		if (score <= kLowLoadThreshold) {
			bestCore = candidate;
			bestLoad = score;
			return true;
		}

		if (bestCore == NULL || score < bestLoad) {
			bestCore = candidate;
			bestLoad = score;
		}
	}

	return false;
}


static inline void
CheckMaskedPackagesMinimumLoad(CPUEntry* cpu, const CPUSet& mask,
	CoreEntry*& bestCore, int32& bestLoad, CoreType type = CORE_TYPE_UNKNOWN)
{
	const int32 kCPUSetArraySize = (SMP_MAX_CPUS + 31) / 32;
	const int32 cpuCount = smp_get_num_cpus();
	PackageEntry* lastPackage = NULL;

	const bool useVisitedBitmask = (gPackageCount <= 128);
	uint64 visitedPackages[2] = {0, 0};

	const bool useMediumBitmask = (!useVisitedBitmask && gPackageCount <= 512);
	uint64 mediumVisited = 0;

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
				if (package != NULL) {
					bool alreadyVisited = false;
					if (useVisitedBitmask) {
						int32 idx = package->ID();
						if (idx >= 0 && idx < 128) {
							int32 word = idx / 64;
							uint64 bitMask = 1ULL << (idx % 64);
							if (visitedPackages[word] & bitMask)
								alreadyVisited = true;
							else
								visitedPackages[word] |= bitMask;
						}
					} else if (useMediumBitmask) {
						int32 slot = package->ID() % 64;
						uint64 bit = 1ULL << slot;
						alreadyVisited = (mediumVisited & bit) != 0;
						if (!alreadyVisited)
							mediumVisited |= bit;
					} else {
						alreadyVisited = (package == lastPackage);
					}
					if (!alreadyVisited) {
						CheckPackageMinimumLoad(cpu, package, &mask, bestCore,
							bestLoad, type);
						lastPackage = package;
					}
				}
			}
		}
	}
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_TOPOLOGY_H
