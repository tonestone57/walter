// AUDIT FIXES: issues 5, 17, 26, 51, 71, 79
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
static void search_local_node(SchedulerNode* node, Action action) {
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
	// Use cpu->fLastLocalPackageIndex (per-CPU) rather than the old
	// core->fLastLocalPackageIndex.  The CoreEntry field was written on every
	// call by every CPU sharing the core, producing false sharing on the core's
	// core's hot read-mostly cache line.  The per-CPU field is private to
	// one CPU so no cross-CPU invalidation occurs.
	//
	// fLastLocalPackageIndex is updated on EVERY iteration, not only on
	// success.  This is intentional: if all packages in the node reject
	// (all busy), the index still advances so the next call starts from a
	// fresh position, maintaining the round-robin guarantee under sustained
	// load.
	if (packagesInNode <= 8) {
		int32 lastIndex = cpu->fLastLocalPackageIndex;
		int32 start = (lastIndex + 1) % packagesInNode;
		int32 finalOffset = lastIndex;
		for (int32 i = 0; i < packagesInNode; i++) {
			// (clarification): fLastLocalPackageIndex is per-CPU.
			// Updating it once per call gives correct round-robin
			// coverage: next call starts from the element after the last
			// one checked.
			int32 offset = start + i;
			if (offset >= packagesInNode)
				offset -= packagesInNode;
			int32 index = nodeBaseIndex + offset;
			if (index >= gPackageCount)
				continue;

			finalOffset = offset;
			if (action(&gPackageEntries[index]))
				break;
		}
		// Update per-CPU field once; plain store is sufficient as this
		// state is private to the current CPU's search context.
		cpu->fLastLocalPackageIndex = finalOffset;
		return;
	}

	// For larger nodes, use logarithmic random sampling.  Duplicate probes
	// become statistically rare once N is large enough.
	//
	// (documentation): The visitedBits array in search_global_random is
	// a fixed 512-byte stack allocation (kStackBitmaskSize == 4096 packages).
	// For systems with >4096 packages deduplication is skipped but the loop
	// still terminates within kMaxAttempts, bounding stack use unconditionally.
	int32 logPackages = 0;
	if (packagesInNode > 1)
		logPackages = fls((uint32)packagesInNode) - 1;

	const int32 samplesToTake = min_c(packagesInNode, 4 + logPackages);
	int32 samplesTaken = 0;
	int32 attempts = 0;
	const int32 kMaxAttempts = samplesToTake * 8;

	// Note: the large-node random path has no visited bitmask,
	// allowing the same package to be probed multiple times within a single
	// call. Use a stack-allocated bitmask for nodes with up to 512 packages
	// to avoid duplicate probes and wasted budget.
	const int32 kLocalStackBitmaskSize = 512;
	const bool canDedup = (packagesInNode <= kLocalStackBitmaskSize);

	if (canDedup && packagesInNode <= 64) {
		uint64 visitedLocal = 0;
		while (samplesTaken < samplesToTake && attempts++ < kMaxAttempts) {
			int32 index =
				nodeBaseIndex +
				(int32)(((uint64)cpu->GetRandom() * packagesInNode) >> 32);
			if (index >= gPackageCount)
				continue;

			int32 localIdx = index - nodeBaseIndex;
			if (localIdx >= 0 && localIdx < 64) {
				uint64 bit = 1ULL << localIdx;
				if (visitedLocal & bit)
					continue;
				visitedLocal |= bit;
			}

			samplesTaken++;
			if (action(&gPackageEntries[index]))
				break;
		}
		return;
	}

	uint64 visitedLocalStack[kLocalStackBitmaskSize / 64];
	if (canDedup) {
		int32 wordsNeeded = (packagesInNode + 63) / 64;
		memset(visitedLocalStack, 0, (size_t)wordsNeeded * sizeof(uint64));
	}

	while (samplesTaken < samplesToTake && attempts++ < kMaxAttempts) {
		int32 index =
			nodeBaseIndex +
			(int32)(((uint64)cpu->GetRandom() * packagesInNode) >> 32);
		if (index >= gPackageCount)
			continue;

		if (canDedup) {
			int32 localIdx = index - nodeBaseIndex;
			if (localIdx >= 0 && localIdx < kLocalStackBitmaskSize) {
				int32 word = localIdx / 64;
				int32 bit = localIdx % 64;
				if (visitedLocalStack[word] & (1ULL << bit))
					continue;
				visitedLocalStack[word] |= (1ULL << bit);
			}
		}

		samplesTaken++;
		if (action(&gPackageEntries[index]))
			break;
	}
}

template <typename Action>
static void search_numa_random(int32 numaID, int32 excludeNode, Action action) {
	const int32 nodeCount = gNodeCount;
	if (nodeCount <= 0)
		return;

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
	int32 samplesToTake = min_c(gRandomSamples, nodeCount);
	int32 samplesTaken = 0;
	int32 attempts = 0;
	const int32 kMaxAttempts = samplesToTake * 8;

	while (samplesTaken < samplesToTake && attempts++ < kMaxAttempts) {
		int32 i = (int32)(((uint64)cpu->GetRandom() * nodeCount) >> 32);
		SchedulerNode* node = &gSchedulerNodes[i];

		if (i == excludeNode || node->NUMAID() != numaID)
			continue;

		samplesTaken++;

		int32 packagesInNode = node->PackageCount();
		if (packagesInNode <= 0)
			continue;

		int32 pkgOffset =
			(int32)(((uint64)cpu->GetRandom() * packagesInNode) >> 32);
		if (action(&gPackageEntries[node->PackageStartIndex() + pkgOffset]))
			break;
	}
}

template <typename Action>
static void search_global_random(Action action) {
	// Note: snapshot gPackageCount once at the start of the function.
	// This ensures consistency if a hot-plug event changes the global count
	const int32 packageCount = gPackageCount;

	// Note: guard packageCount == 0 before computing samplesToTake
	// and entering the while loop. min_c(gRandomSamples, 0) == 0 so the
	// loop would not execute, but the ASSERT and wordsNeeded computation
	// below could misbehave with packageCount == 0.
	if (packageCount <= 0)
		return;

	// Note: kStackBitmaskSize covers 4096 packages (512 bytes on the
	// stack).  init() enforces gPackageCount <= 4096.  Assert that the runtime
	// value never exceeds our compile-time allocation so an accidental removal
	// of the init() cap does not silently cause out-of-bounds writes.
	ASSERT(packageCount <= 4096);

	int32 samplesToTake = min_c(gRandomSamples, packageCount);
	int32 samplesTaken = 0;
	int32 attempts = 0;
	const int32 kMaxAttempts = samplesToTake * 8;

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());

	// Bitmask for tracking visited packages to avoid collisions.
	// For systems with <= 64 packages, use a single uint64 bitmask (fast path).
	// the fast-path shift `1ULL << i` where
	// i is in [0, packageCount-1] with packageCount <= 64 means i is in
	// [0, 63].  Shifting a 64-bit literal by 63 is defined behavior.
	// No overflow is possible.  No code change required.
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

	// the previous kStackBitmaskSize of 1024 meant that packages
	// in the range [1024, 4096) fell into a coarse stripe-based fallback
	// that visited at most 1 package per 64-package band, severely
	// under-sampling large systems.  gPackageCount is capped at 4096, so a
	// 512-byte (4096-bit) bitmask covers the entire valid range without any
	// stripe approximation.  512 bytes is acceptable on a kernel stack that
	// is usually 16 KB.
	const int32 kStackBitmaskSize = 4096;
	uint64 visitedBits[kStackBitmaskSize / 64];

	// Note: use snapshotted packageCount.
	// zero only the words needed for packageCount instead of
	// always zeroing all 512 bytes (64 uint64s).  For a 65-package system
	// this reduces unnecessary cache-line writes from 512 bytes to 16 bytes.
	int32 wordsNeeded =
		min_c((packageCount + 63) / 64, (int32)(kStackBitmaskSize / 64));
	memset(visitedBits, 0, (size_t)wordsNeeded * sizeof(uint64));

	while (samplesTaken < samplesToTake && attempts++ < kMaxAttempts) {
		int32 i = (int32)(((uint64)cpu->GetRandom() * packageCount) >> 32);

		// With kStackBitmaskSize == 4096 and gPackageCount <= 4096 every
		// valid index i fits inside the bitmask.  The out-of-range branch
		// is retained as a safety net in case the cap ever changes.
		int32 word = i / 64;
		int32 bit = i % 64;

		if (i < kStackBitmaskSize) {
			if ((visitedBits[word] & (1ULL << bit)) != 0)
				continue;
			visitedBits[word] |= (1ULL << bit);
		}
		// For indices >= kStackBitmaskSize (unreachable today) allow the
		// probe without deduplication; at worst we visit a package twice.
		samplesTaken++;

		if (action(&gPackageEntries[i]))
			break;
	}
}


static inline bool CheckPackageMinimumLoad(CPUEntry* cpu, PackageEntry* entry,
										   const CPUSet* mask,
										   CoreEntry*& bestCore,
										   int32& bestLoad,
										   CoreType type = CORE_TYPE_UNKNOWN) {
	CoreEntry* candidate = entry->PeekMinimumLoadCore(cpu, mask, type);

	if (candidate != NULL) {
		int32 score = candidate->GetScore();

		// Load-Threshold Short Circuit: If we find a core with very low load
		// (e.g., < 15% of kMaxLoad), accept it immediately to save cycles.
		const int32 kLowLoadThreshold = (kMaxLoad * 15) / 100;
		if (score <= kLowLoadThreshold) {
			bestCore = candidate;
			bestLoad = score;
			return true;  // callers must check this return value
		}

		if (bestCore == NULL || score < bestLoad) {
			bestCore = candidate;
			bestLoad = score;
		}
	}

	return false;  // continue searching
}


static inline void CheckMaskedPackagesMinimumLoad(
	CPUEntry* cpu, const CPUSet& mask, CoreEntry*& bestCore, int32& bestLoad,
	CoreType type = CORE_TYPE_UNKNOWN) {
	const int32 kCPUSetArraySize = (SMP_MAX_CPUS + 31) / 32;
	const int32 cpuCount = smp_get_num_cpus();
	PackageEntry* lastPackage = NULL;

	// Note: the previous deduplication only caught *consecutive*
	// duplicate packages. Two CPU IDs in non-adjacent affinity mask bits
	// can belong to the same package and be scanned twice, wasting time and
	// skewing the minimum-load result toward that package. Use a proper
	// visited bitmask for small systems and a hash-based set for larger ones.
	//
	// For systems with <= 128 packages (covers all practical single/dual-socket
	// servers), use a two-word uint64 bitmask keyed on package ID.
	// For larger systems the consecutive-duplicate check is retained as a
	// lightweight approximation (full dedup would require heap allocation).
	const bool useVisitedBitmask = (gPackageCount <= 128);
	uint64 visitedPackages[2] = {0, 0};	 // covers package IDs 0..127

	// Note: already present in original. Additional fix for
	// gPackageCount > 128 fallback: consecutive-duplicate suppression
	// misses non-adjacent duplicates.
	// Optimization: Extend bitmask to 512 packages.
	// This covers virtually all server hardware precisely.
	const bool useExtendedBitmask =
		(!useVisitedBitmask && gPackageCount <= 512);
	uint64 extendedVisited[8];	// 8 * 64 = 512 bits
	if (useExtendedBitmask)
		memset(extendedVisited, 0, sizeof(extendedVisited));

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

			CPUEntry* cpuEntry = CPUEntry::GetCPU(cpuID);
			if (cpuEntry == NULL)
				continue;
			CoreEntry* cpuCore = cpuEntry->Core();
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
					} else if (useExtendedBitmask) {
						int32 idx = package->ID();
						if (idx >= 0 && idx < 512) {
							int32 word = idx / 64;
							uint64 bitMask = 1ULL << (idx % 64);
							if (extendedVisited[word] & bitMask)
								alreadyVisited = true;
							else
								extendedVisited[word] |= bitMask;
						}
					} else {
						// Fallback: consecutive-duplicate suppression only.
						alreadyVisited = (package == lastPackage);
					}
					if (!alreadyVisited) {
						if (CheckPackageMinimumLoad(cpu, package, &mask,
													bestCore, bestLoad, type)) {
							return;	 // Short-circuit: found a core with very
									 // low load.
						}
						lastPackage = package;
					}
				}
			}
		}
	}
}

}  // namespace Scheduler

#endif	// KERNEL_SCHEDULER_TOPOLOGY_H
