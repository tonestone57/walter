/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
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

	const int kMaxLocalAttempts = 4;
	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
	for (int i = 0; i < kMaxLocalAttempts; i++) {
		// Multiplicative random mapping to avoid expensive modulo
		int32 index = nodeBaseIndex
			+ (int32)(((uint64)cpu->GetRandom() * packagesInNode) >> 32);
		if (action(&gPackageEntries[index]))
			break;
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
		if (action(&gPackageEntries[i]))
			break;
	}
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_TOPOLOGY_H
