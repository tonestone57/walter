# Haiku Scheduler Audit & Optimization Report (GCC 13 & Portability)

## 1. GCC 13 Compatibility & Standardized Atomics
- **Standardized Portability Layer**: Established a robust set of atomic helpers in `scheduler_common.h` (`scheduler_atomic_*64`, `LoadAcquire`, `StoreRelease`, etc.) using explicit `reinterpret_cast<int64 volatile*>` to satisfy GCC 13's stricter type checking.
- **Type-Safe Wrappers**: Implemented `LoadAcquire`, `StoreRelease`, `AddRelease`, and `SubAcquireRelease` to provide a consistent, type-safe interface for `int32` atomic operations across the scheduler subsystem.
- **Atomic Pointer Safety**: Refined `atomic_pointer_*` templates to leverage standardized helpers, ensuring proper 32/64-bit behavior and memory barriers.

## 2. 32-bit & 64-bit Portability
- **Alignment Attributes**: Enforced 8-byte alignment using `__attribute__((aligned(8)))` for all 64-bit variables accessed via atomic operations (e.g., `fStolenTime`, `fVirtualRuntime`, `gIdleMask`, `gIdleNodeMask`). This ensures atomic safety on 32-bit platforms where unaligned 64-bit accesses are non-atomic.
- **Native Masking**: Utilized `native_cpu_mask_t` and portable bit manipulation intrinsics (`scheduler_ctz`, `scheduler_ffs64`, `scheduler_popcount`) to handle varying CPU mask widths and topologies.

## 3. Performance Optimizations
- **Hybrid RunQueue Structure**: Refactored `ThreadRunQueue` to a hybrid data structure combining dual array-based binary min-heaps for EEVDF deadline sorting with per-priority circular doubly-linked lists and a priority bitmap. This enables $O(1)$ retrieval of threads by priority while maintaining $O(\log N)$ heap complexity for deadline-based scheduling.
- **Scalable Priority Boosting**: Optimized the starvation scanner to utilize the hybrid RunQueue's priority bitmap. Implemented a per-priority work budget (`kMaxPrioritiesToCheckPerQueue`) to strictly bound scan overhead, replacing the previous $O(N)$ linear scan of heaps with $O(1)$ lookup per priority level.
- **Modular Core Ownership**: Implemented a round-robin ownership model for core-level starvation scans using a modular index calculation (`boostEpoch % coreCPUCount`). This prevents multiple CPUs on an SMT core from contending on the shared Core RunQueue lock simultaneously.
- **Deep Timestamp Propagation**: Refactored the scheduling hot path (`reschedule()`) to capture `system_time()` once and propagate the `now` timestamp through accounting, load tracking, and quantum calculation. This eliminates redundant hardware timer reads in the kernel's most frequent execution path.
- **Thread Coloring Refinement**: Optimized core selection logic in `low_latency.cpp` and `power_saving.cpp` to skip redundant type-filtered searches on homogeneous systems, reducing overhead.
- **Lockless Hints**: Implemented `CoreEntry::HasHighPriorityThread()` using lockless run-queue bitmap reads, improving quantum calculation performance by avoiding `TryLockRunQueue` contention.
- **Bounded CAS Loops**: Hardened E-core selection loops in `power_saving.cpp` with a retry limit (16) and `cpu_pause()` to prevent indefinite spinning under high contention.

## 4. Bug Fixes & Robustness
- **32/64-bit Shift Domain Safety**: Hardened bitwise modular calculations in `UpdatePriorityBoostScalable` by explicitly clamping shift domains to the bit width of `native_cpu_mask_t`, preventing undefined behavior on 32-bit and 64-bit platforms.
- **Interaction State Reliability**: Resolved a race condition in `scheduler_update_interaction_state` by ensuring the interaction timer is only armed if the DPC is successfully queued. Implemented explicit recovery paths (clearing `sDPCPending`) for DPC queue saturation to prevent permanent loss of timer resolution.
- **fRescheduleCount Wrap-around Protection**: Implemented wrap-around safety for the reschedule counter used in starvation scanning, ensuring stable non-zero epoch boundaries at the `UINT32_MAX -> 0` transition to prevent correlated scan bursts across all CPUs.
- **RunQueue Hardening**: Implemented stale pointer detection for the `fBest` cache in `RunQueue.h`. Added an authoritative rescan in `PeekBest()` to guarantee finding a thread if the queue is non-empty, even under high contention.
- **Hot-Unplug Safety**: Added explicit `NULL` guards for `Core()`, `Package()`, and `Node()` dereferences throughout the scheduler and load balancer to prevent kernel panics during CPU hot-unplug events.
- **Counter Consistency**: Verified and corrected various atomic counter updates (e.g., `fThreadCount`, `gTotalRunnableThreads`) to ensure symmetric increments/decrements and avoid leaks.
- **IRQ Draining**: Refined the IRQ draining logic in `CPUEntry::Stop` with a 1000-iteration safety bound and progress verification to prevent infinite loops.

## 5. Scalability & Cache Efficiency (Phase 2 Audit)
- **De-centralized RCU Management**: Moved RCU callback queues and spinlocks from global scope into the `CPUEntry` class. This eliminates a primary point of global contention during thread lifecycle events and interaction state updates on systems with high core counts.
- **Global Interaction State Alignment**: Grouped `sLastInteractionTime`, `sDPCPending`, `sTimerArmed`, and `sPendingDPCTarget` into a `struct InteractivityState` explicitly aligned to 64 bytes (`CACHE_LINE_ALIGN`). This eliminates false sharing (cache invalidation loops) between CPUs frequently updating these counters.
- **Hierarchical Work-Stealing Sampling**: Refactored `search_global_random` to utilize a Node -> Package sampling strategy. By first selecting a random `SchedulerNode` before probing a `PackageEntry`, the distribution of stealing probes is significantly improved on large NUMA systems, favoring better coverage of all memory domains.
- **TakeSnapshot Consolidation**: Moved the `TakeSnapshot` helper into `scheduler_common.h` as an inline function, eliminating redundant re-definitions and improving the consistency of load-balancing snapshots across translation units.

## 6. Maintenance & Style
- **Technical Commentary**: Updated "Issue XX" documentation and technical commentary to maintain historical context and explain complex synchronization patterns.
- **Style Conformance**: Normalized indentation (tabs) and vertical whitespace (double-newlines between functions) according to Haiku kernel standards.
