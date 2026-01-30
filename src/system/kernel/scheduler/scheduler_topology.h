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

	// SchedulerNode represents a 64-package block in the dense gPackageEntries array.
	int32 nodeBaseIndex = node->NodeIndex() * 64;

	// Ensure we don't go out of bounds of gPackageEntries
	if (nodeBaseIndex >= gPackageCount)
		return;

	int32 packagesInNode = min_c(64, gPackageCount - nodeBaseIndex);
	if (packagesInNode <= 0)
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
	// Cap at 4096 packages (512 bytes on stack).
	const int32 kMaxPackages = 4096;
	uint64 visitedBits[kMaxPackages / 64];

	// Only clear the portion of the bitmask we actually need.
	// This ensures minimal overhead for small systems.
	int32 packages = gPackageCount;
	if (packages > kMaxPackages)
		packages = kMaxPackages;

	int32 wordsToClear = (packages + 63) / 64;
	memset(visitedBits, 0, wordsToClear * sizeof(uint64));

	while (samplesTaken < samplesToTake && attempts++ < kMaxAttempts) {
		// Multiplicative random mapping to avoid expensive modulo
		int32 i = (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32);

		// Avoid checking the same package twice using the bitmask
		int32 word = i / 64;
		int32 bit = i % 64;

		if (word >= wordsToClear)
			continue;

		if ((visitedBits[word] & (1ULL << bit)) != 0)
			continue;

		visitedBits[word] |= (1ULL << bit);
		samplesTaken++;

		action(&gPackageEntries[i]);
	}
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_TOPOLOGY_H
