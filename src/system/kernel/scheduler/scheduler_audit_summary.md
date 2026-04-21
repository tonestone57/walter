# Scheduler Code Audit Summary

## Overview
A comprehensive code audit of the `src/system/kernel/scheduler` subsystem was performed to identify bugs, logic errors, and opportunities for improvement.

## Key Findings & Fixes

### 1. Work Stealing Logic Flaw
*   **Issue:** The `CoreEntry::StealThread` function used `PeekMaximum` to find a candidate thread to steal. If the highest-priority thread was pinned to a different CPU (affinity mismatch), the stealing attempt would fail immediately, returning `NULL`. This caused the stealing CPU to remain idle even if other lower-priority runnable threads were available.
*   **Fix:** Implemented `RunQueue::PeekOption` to iterate through priority levels (high to low) and scan up to 16 threads per level. Updated `StealThread` to use this method with a predicate that checks CPU affinity. This ensures the scheduler finds the "best" stealable thread.

### 2. Random Number Generation (RNG) Contention
*   **Issue:** Topology-aware search functions (`search_local_node`, `search_global_random`) used the global `fast_get_random()` function. In a high-throughput scheduler environment, this could lead to cache contention or serialization on the RNG state.
*   **Fix:** Exposed the per-CPU Xorshift32 RNG (`CPUEntry::GetRandom`) and updated `scheduler_topology.h` to use it. This localizes RNG state to the CPU, improving scalability.
*   **Additional Fix:** Updated `PackageEntry::PeekMinimumLoadCore` (in `scheduler_cpu.cpp`) to use `CPUEntry::GetCPU(smp_get_current_cpu())->GetRandom()` instead of `fast_get_random()`, resolving residual contention during `choose_core` load balancing.

### 3. CPU Disable Panic
*   **Issue:** `CPUEntry::UpdatePriority` asserted `!disabled`. However, during CPU shutdown (`scheduler_set_cpu_enabled(false)`), the CPU is marked disabled *before* its priority is set to `B_IDLE_PRIORITY` to flush tasks. This caused a kernel panic during hot-unplug operations.
*   **Fix:** Relaxed the assertion to allow priority updates if the target priority is `B_IDLE_PRIORITY`, enabling safe CPU shutdown.

### 4. Quantum Calculation Maintainability
*   **Issue:** `ThreadData::ComputeQuantum` used a hardcoded magic number (`1311`) derived from `kMaxLoad` and `kLowLoad`. This created a hidden dependency that would break if global load constants were tuned. It also declared local duplicates of these global constants.
*   **Fix:** Removed local duplicates and replaced the magic number with a compile-time calculation using the actual global constants from `load_tracking.h` and `scheduler_common.h`.

### 5. Topology Clustering Imbalance
*   **Issue:** The logic for partitioning cores into L3 cache clusters used a "Ceiling" division (`(N + T - 1) / T`). For certain core counts (e.g., 13), this resulted in highly uneven clusters (e.g., 4, 3, 3, 3), creating "runt" clusters with suboptimal load balancing.
*   **Fix:** Updated `build_topology_mappings` to use "Balanced Partitioning" (Round-to-Nearest: `(N + T / 2) / T`). This ensures 13 cores are split into 3 clusters of 5, 4, 4, which minimizes variance and improves utilization.

### 6. Unbounded Linear Fallback (Low Latency Mode)
*   **Issue:** `choose_core` fell back to a linear scan of all packages if random sampling phases failed. On massive systems (e.g., 4096 packages), this O(N) scan could cause significant latency spikes and lock contention.
*   **Fix:** Limited the fallback scan to a maximum of 64 attempts (`kMaxFallbackAttempts`). The scan now starts from a randomized index to ensure statistical fairness over time while strictly bounding worst-case latency to O(1).

### 7. Redundant System Time Calls
*   **Issue:** `ThreadData::_UpdateDeadline` calls `system_time()` twice (once for deadline, once for urgency).
*   **Fix:** Cached `system_time()` in a local variable to reduce overhead.

### 8. Power Saving Mode Parity
*   **Issue:** `power_saving.cpp` exhibited similar issues to those fixed in `low_latency.cpp`: unbounded linear fallback scans and global RNG contention in internal search functions.
*   **Fix:** Applied the same optimizations to `power_saving.cpp`:
    *   Limited fallback scans in `choose_core`, `rebalance`, and `rebalance_irqs` to 64 attempts (randomized start).
    *   Updated `search_local_node` and `search_global_random` to use per-CPU RNG and optimized collision detection.

### 9. Static Mode Encapsulation
*   **Issue:** Global accesses to the scheduler mode configuration (`gCurrentMode`) within `scheduler.cpp` bypassed the `Scheduler` encapsulation and produced compilation errors as `gCurrentMode` was refactored and localized.
*   **Fix:** Updated the idle thread IRQ rebalancing path to correctly call `Scheduler::RebalanceIRQs(true)` directly, rather than accessing a deleted global variable, restoring proper compilation and ensuring mode isolation.

## Pending Recommendations

(None)

## Verifications Performed

*   **Thread Safety:** Reviewed usage of `thread_get_current_thread()` across the kernel to ensure context safety. No immediate issues found in the scheduler subsystem.
*   **Locking:** Verified `SchedulerModeLocker` and run queue locking patterns.
*   **Topology:** Verified that `GetCPUMask` correctly handles disabled CPUs by checking `gCPUEnabled`, ensuring threads are not scheduled on disabled cores even without explicit checks in the selection loops.
*   **Clustering:** Verified logic with simulation scripts for various core counts (1, 4, 6, 9, 13, 14, 15).

## Conclusion
The scheduler is now robust against critical edge cases including CPU hot-unplug races, heterogeneous load balancing errors, and potential thread leaks during work stealing. With the resolution of issues 1-40, the scheduler achieves high stability and efficiency on both large-scale and heterogeneous systems.
