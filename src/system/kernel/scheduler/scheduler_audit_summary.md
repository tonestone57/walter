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

### 3. CPU Disable Panic
*   **Issue:** `CPUEntry::UpdatePriority` asserted `!disabled`. However, during CPU shutdown (`scheduler_set_cpu_enabled(false)`), the CPU is marked disabled *before* its priority is set to `B_IDLE_PRIORITY` to flush tasks. This caused a kernel panic during hot-unplug operations.
*   **Fix:** Relaxed the assertion to allow priority updates if the target priority is `B_IDLE_PRIORITY`, enabling safe CPU shutdown.

### 4. Quantum Calculation Maintainability
*   **Issue:** `ThreadData::ComputeQuantum` used a hardcoded magic number (`1311`) derived from `kMaxLoad` and `kLowLoad`. This created a hidden dependency that would break if global load constants were tuned. It also declared local duplicates of these global constants.
*   **Fix:** Removed local duplicates and replaced the magic number with a compile-time calculation using the actual global constants from `load_tracking.h` and `scheduler_common.h`.

### 5. Topology Clustering Imbalance
*   **Issue:** The logic for partitioning cores into L3 cache clusters used a "Ceiling" division (`(N + T - 1) / T`). For certain core counts (e.g., 13), this resulted in highly uneven clusters (e.g., 4, 3, 3, 3), creating "runt" clusters with suboptimal load balancing.
*   **Fix:** Updated `build_topology_mappings` to use "Balanced Partitioning" (Round-to-Nearest: `(N + T / 2) / T`). This ensures 13 cores are split into 3 clusters of 5, 4, 4, which minimizes variance and improves utilization.

## Further Recommendations (Pending)

### 1. Residual RNG Contention
*   **Issue:** While `scheduler_topology.h` was updated, `PackageEntry::PeekMinimumLoadCore` (in `scheduler_cpu.cpp`) still calls the global `fast_get_random()`. This remains a contention point during `choose_core` operations.
*   **Recommendation:** Update `PackageEntry::PeekMinimumLoadCore` to use `CPUEntry::GetCPU(smp_get_current_cpu())->GetRandom()`.

### 2. Redundant System Time Calls
*   **Issue:** `ThreadData::_UpdateDeadline` calls `system_time()` twice (once for deadline, once for urgency).
*   **Recommendation:** Cache `system_time()` in a local variable to reduce overhead.

### 3. Unbounded Linear Fallback
*   **Issue:** `choose_core` falls back to a linear scan of all packages if random sampling fails. On massive systems, this could be expensive.
*   **Recommendation:** Limit the fallback scan to a fixed number of attempts.

## Verifications Performed

*   **Thread Safety:** Reviewed usage of `thread_get_current_thread()` across the kernel to ensure context safety. No immediate issues found in the scheduler subsystem.
*   **Locking:** Verified `SchedulerModeLocker` and run queue locking patterns.
*   **Topology:** Verified that `GetCPUMask` correctly handles disabled CPUs by checking `gCPUEnabled`, ensuring threads are not scheduled on disabled cores even without explicit checks in the selection loops.
*   **Clustering:** Verified logic with simulation scripts for various core counts (1, 4, 6, 9, 13, 14, 15).

## Conclusion
The scheduler is now more robust against edge cases (CPU pinning, hot-unplug) and has improved scalability characteristics due to optimized RNG usage. The new clustering logic ensures better topology balance.
