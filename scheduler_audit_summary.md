# Haiku Scheduler Audit & Refinement Summary (2025)

## 1. GCC 2.95 Compatibility & Portability Layer
*   **Established `scheduler_common.h` Portability Layer:**
    *   Defined `native_cpu_mask_t` (32-bit or 64-bit depending on architecture) to support large core counts while remaining efficient on 32-bit systems.
    *   Implemented `scheduler_atomic_*` wrappers for architecture-independent atomic bitwise operations.
    *   Added `scheduler_ctz` (Count Trailing Zeros), `scheduler_popcount`, and `scheduler_ffs64` as portable alternatives to GCC built-ins not available in 2.95.
    *   Fixed `scheduler_ctz(0)` to correctly return `-1`.
    *   Implemented template-based `atomic_pointer_get/set/test_and_set` to handle atomic pointer access across 32-bit and 64-bit platforms with explicit template arguments required by GCC 2.95.

## 2. Race Condition & TOCTOU Fixes
*   **Authoritative `RemoveCPU` Idle Transition (Issue 96):**
    *   Updated `CoreEntry::RemoveCPU` to check the authoritative heap key (holding `fCPULock`) instead of a racy flag.
    *   Only decrements `fIdleCPUCount` if the CPU was actually idle at the point of removal.
    *   Ensures `RemoveIdleCore` is called only when the core was definitively idle, preserving package-level accounting.
*   **Racy `fCore` Dereferences (Issue 7):**
    *   Modified `ThreadData::GoesAway` and `Dies` to use a single `atomic_pointer_get` snapshot of `fCore`. This prevents null-dereference panics if `fCore` is cleared by a concurrent `MigrateTo` call between the check and use.
*   **Atomic RunQueue `fBest` Maintenance (Issue 22/46):**
    *   Updated `PushFront`, `PushBack`, and `Remove` in `RunQueue.h` to read `fBest` once and validate its priority bucket under the run-queue lock. This prevents usage of stale or dangling pointers if `fBest` was updated locklessly on another CPU.
*   **IPI dead-code Race (Issue 16/84):**
    *   Fixed a logic error in `scheduler.cpp::enqueue` where an early return skipped ICI (IPI) dispatch and listener notifications, causing indefinite scheduling delays for woken threads.

## 3. Scalability & Efficiency Improvements
*   **Bitmask-based Core Indexing (Issue 59):**
    *   Replaced the fragile `fNextCoreLocalIndex` counter in `CoreEntry` with an atomic bitmask `fLocalIndices`. This guarantees unique index assignment even during concurrent CPU hot-plugging/enabling.
    *   Used `scheduler_popcount` in `UpdatePriorityBoostScalable` to map these bitmask indices to a dense range for fair round-robin run-queue scanning.
*   **3-Phase Work Stealing:**
    *   Implemented a hierarchical strategy: 1) Local Sibling (L2/L3 domain), 2) Local NUMA node, 3) Global Random.
    *   Reduces cross-socket traffic and cache misses by preferring closer victims.
*   **Improved RNG Entropy (Issue 44):**
    *   Enhanced `CPUEntry` RNG seeding with `system_time()`, `this` pointer, and CPU ID using a 64-bit mixer.
    *   Staggered context switch boundaries (`fRescheduleCount`) across CPUs to reduce correlated lock spikes.

## 4. Logical Correctness & Architecture Safety
*   **64-bit Alignment:** Ensured all 64-bit members accessed via atomic64 operations (e.g., `fCombinedLoad`, `fStolenTime`, `fVirtualRuntime`, `gIdleMask`) are 8-byte aligned to prevent bus errors or tearing on 32-bit platforms.
*   **Division-by-Zero Guards (Issue 76):** Added checks for zero capacity and zero score factor in `ThreadData::ComputeQuantum` and mode initialization.
*   **Wait-free Idle Discovery:** Optimized `PeekMinimumLoadCPU` to use bitmask scans for idle CPUs before falling back to the priority heap.
*   **Batch Processing Fix:** Corrected `scheduler_on_team_foreground_changed` to properly handle thread list iteration at batch boundaries, ensuring no threads are skipped or double-processed.
*   **Authoritative PeekBest (Issue 90):** Implemented a full scan fallback in `RunQueue::PeekBest` to guarantee a result from a non-empty queue if lookahead heuristics are exhausted.

## 5. Scheduler Mode Strategy Refinements
*   **Spread Strategy (Low Latency):** Enhanced `choose_core` to respect thread coloring (heterogeneous types), NUMA domains, and cache expiry.
*   **Packing Strategy (Power Saving):** Updated `GetIdleCorePacking` to prefer neighbors of active cores, maximizing the number of fully-powered-down packages.
*   **Interactivity Scoring:** Verified that virtual deadline updates correctly floor slices at `gDeadlineBucketSize / 4` to prevent starvation of interactive tasks (Issue 37).
