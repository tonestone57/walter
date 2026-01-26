/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_TOPOLOGY_H
#define KERNEL_SCHEDULER_TOPOLOGY_H


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
	for (int i = 0; i < kMaxLocalAttempts; i++) {
		int32 index = nodeBaseIndex
			+ (fast_get_random<uint32>() % packagesInNode);
		action(&gPackageEntries[index]);
	}
}


template <typename Action>
static void
search_global_random(Action action)
{
	const int32 kMaxRandomSamples = 256;
	int32 visited[kMaxRandomSamples];
	int32 samplesToTake = min_c(gRandomSamples, kMaxRandomSamples);
	int32 samplesTaken = 0;
	int32 attempts = 0;
	const int32 kMaxAttempts = samplesToTake * 2;

	while (samplesTaken < samplesToTake && attempts++ < kMaxAttempts) {
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

		action(&gPackageEntries[i]);
	}
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_TOPOLOGY_H
