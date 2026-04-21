# Scheduler Audit Findings

## Critical Bugs (Fixed)

### 1. Work Stealing Core Mismatch
**Severity:** Critical
**Fixed In:** `scheduler-audit-fixes` branch
**Description:**
Stolen threads retained their old `fCore` pointer, causing assertion failures and incorrect load tracking.
**Fix:** Introduced `ThreadData::MigrateTo` to atomically update core affinity and transfer load.

### 2. Thread Leak in `ChooseNextThread`
**Severity:** Critical
**Fixed In:** `scheduler-audit-fixes` branch
**Description:**
Threads stolen but not immediately scheduled (due to priority) were not added to the new core's run queue, effectively disappearing from the scheduler.
**Fix:** Added logic to re-enqueue "floating" threads.

### 3. Race Condition in `_TryStealWork`
**Severity:** Critical
**Fixed In:** `scheduler-audit-fixes` branch
**Description:**
Load transfer was performed after releasing the victim's lock.
**Fix:** Moved `MigrateTo` call inside the critical section.

## Performance Improvements

### 4. Inefficient Topology Search
**Severity:** Medium
**Fixed In:** `scheduler-audit-optimizations` branch
**Description:**
`search_global_random` and `search_local_node` continue iterating and generating random numbers even after a target has been found.
**Fix:** Updated templates to accept a predicate returning `bool` (stop/continue) to allow early exit.

### 5. Profiler Stack Safety
**Severity:** Low
**Fixed In:** `scheduler-audit-optimizations` branch
**Description:**
If `EnterFunction` hits the stack limit, it returns without pushing. `ExitFunction` blindly pops, potentially corrupting the stack tracking (underflow/mismatch).
**Fix:** Added logic to `ExitFunction` to prevent underflow.

## Minor Issues & Observations

### 6. `sPackageToNode` Memory Management
**Severity:** Minor / Style
**Fixed In:** `scheduler-audit-fixes-cleanup` branch
**Description:** `new` without `ArrayDeleter` created potential memory leak on partial failure.
**Fix:** Added `ArrayDeleter` for `sPackageToNode`.

### 7. Stack-Based Collision Detection Limit (Expanded)
**Severity:** Low
**Fixed In:** `scheduler-audit-fixes` branch
**Description:** `search_global_random` was limited to 1024 packages.
**Fix:** Increased `kStackBitmaskSize` to 4096 and optimized bitmask zeroing to match `gPackageCount`.

### 8. Mode-Specific Logic
**Status:** Verified.
`low_latency` and `power_saving` modes use consistent logic for core selection and rebalancing. `power_saving` aggressively packs small tasks.

### 9. Load Tracking Constants
**Status:** Verified.
Dynamic calculation of `kRangeReciprocal` is correct.

### 10. Load Tracking Overflow
**Status:** Verified.
`compute_load` logic in `load_tracking.h` guards against overflow for `n > 10`.

### 11. RunQueue Fairness (Improved)
**Status:** Verified.
**PeekBest** now searches up to 3 non-empty priority levels to allow lower-priority threads with significantly earlier deadlines to preempt, while maintaining O(1) complexity (Issue 40).

### 12. Hot-Unplug Robustness
**Status:** Fixed.
Multiple race conditions and NULL pointer dereferences during CPU hot-unplug were resolved, including stolen thread recovery, Core pointer guarding, and re-enqueue prevention for dying threads (Issues 14, 23, 24, 29, 30, 32, 33).

### 13. Heterogeneous Scheduling Accuracy
**Status:** Fixed.
Improved E-core utilization by switching from capacity-normalized scores to raw load for threshold guards, and enforced thread coloring on idle core selection (Issues 5, 39).

### 14. Locking Hierarchy
**Status:** Verified.
Locking order `Core -> CPU` is consistently respected. `TryLock` is used for cross-core stealing. Deadlocks are structurally prevented.

### 13. State Transitions & Affinity
**Status:** Verified.
`CPUGoesIdle` / `WakesUp` transitions use atomic operations correctly. `ChooseCoreAndCPU` respects affinity masks and correctly panics on invalid configurations (which shouldn't occur due to upstream checks).
