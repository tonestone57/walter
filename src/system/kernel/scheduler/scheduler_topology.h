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

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());

	// For small nodes (<=8 packages), use a Rotational Linear Scan.  Starting
	// from the last searched position improves coverage and reduces collisions
	// between CPUs searching the same node.
	//
	// Fix #14: Use cpu->fLastLocalPackageIndex (per-CPU) rather than the old
	// core->fLastLocalPackageIndex.  The CoreEntry field was written on every
	// call by every CPU sharing the core, producing false sharing on the core's
	// hot read-mostly cache line.  The per-CPU field is private to one CPU so
	// no cross-CPU invalidation occurs.
	if (packagesInNode <= 8) {
		int32 start = (cpu->fLastLocalPackageIndex + 1) % packagesInNode;
		for (int32 i = 0; i < packagesInNode; i++) {
			int32 offset = start + i;
			if (offset >= packagesInNode)
				offset -= packagesInNode;
			int32 index = nodeBaseIndex + offset;
			if (index >= gPackageCount)
				continue;
			cpu->fLastLocalPackageIndex = offset;
			if (action(&gPackageEntries[index]))
				break;
		}
		return;
	}

	// For larger nodes, use logarithmic random sampling.  Duplicate probes
	// become statistically rare once N is large enough.
	//
	// Fix #2 (documentation): The visitedBits array in search_global_random is
	// a fixed 128-byte stack allocation (kStackBitmaskSize == 1024 packages).
	// For systems with >1024 packages deduplication is skipped but the loop
	// still terminates within kMaxAttempts, bounding stack use unconditionally.
	int32 logPackages = 0;
	if (packagesInNode > 1)
		logPackages = 31 - __builtin_clz(packagesInNode);

	const int kMaxLocalAttempts = min_c(packagesInNode, 4 + logPackages);
	for (int i = 0; i < kMaxLocalAttempts; i++) {
		int32 index = nodeBaseIndex
			+ (int32)(((uint64)cpu->GetRandom() * packagesInNode) >> 32);
		if (index >= gPackageCount)
			continue;
		if (action(&gPackageEntries[index]))
			break;
	}
}


template <typename Action>
static void
search_global_random(Action action)
{
	int32 samplesToTake = min_c(gRandomSamples, gPackageCount);
	int32 samplesTaken = 0;
	int32 attempts = 0;
	const int32 kMaxAttempts = samplesToTake * 8;

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());

	// Bitmask for tracking visited packages to avoid collisions.
	// For systems with <= 64 packages, use a single uint64 bitmask (fast path).
	if (gPackageCount <= 64) {
		uint64 visitedBits = 0;
		while (samplesTaken < samplesToTake && attempts++ < kMaxAttempts) {
			int32 i = (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32);

			if ((visitedBits & (1ULL << i)) != 0)
				continue;
			visitedBits |= (1ULL << i);

			samplesTaken++;
			if (action(&gPackageEntries[i]))
				break;
		}
		return;
	}

	// Use a smaller fixed buffer on the stack (128 bytes = 1024 packages)
	// which covers >99% of systems. For massive systems, we skip collision
	// detection for indices beyond 1024 to save stack space.
	const int32 kStackBitmaskSize = 1024;
	uint64 visitedBits[kStackBitmaskSize / 64];

	// Always zero the entire array. The conditional initialization above
	// only cleared as many words as needed for gPackageCount, leaving words
	// beyond that range uninitialized. Any subsequent access with an index
	// in [cleared_range, kStackBitmaskSize) read garbage, causing spurious
	// "already visited" skips. Zeroing all 128 bytes unconditionally is
	// negligible on the scheduler hot path.
	memset(visitedBits, 0, sizeof(visitedBits));

	while (samplesTaken < samplesToTake && attempts++ < kMaxAttempts) {
		// Multiplicative random mapping to avoid expensive modulo
		int32 i = (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32);

		// Avoid checking the same package twice using the bitmask
		int32 word = i / 64;
		int32 bit = i % 64;

		// Deduplication: skip packages already probed this call (within the
		// stack bitmask range).
		if (i < kStackBitmaskSize) {
			if ((visitedBits[word] & (1ULL << bit)) != 0)
				continue;	// Duplicate within bitmask range: do NOT count.
			visitedBits[word] |= (1ULL << bit);

			// Only count unique probes towards the statistical coverage goal.
			samplesTaken++;
		}
		// For indices beyond kStackBitmaskSize we cannot deduplicate cheaply,
		// but we still count the sample to ensure we eventually terminate
		// based on samplesToTake. This ensures statistical coverage without
		// infinite probing on massive systems.
		if (i >= kStackBitmaskSize)
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

		// Load-Threshold Short Circuit: If we find a core with very low load
		// (e.g., < 15% of kMaxLoad), accept it immediately to save cycles.
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
					CheckPackageMinimumLoad(cpu, package, &mask, bestCore,
						bestLoad, type);
					lastPackage = package;
				}
			}
		}
	}
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_TOPOLOGY_H
