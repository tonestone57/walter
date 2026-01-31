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
**Location:** `src/system/kernel/scheduler/scheduler_topology.h`
**Description:**
`search_global_random` and `search_local_node` continue iterating and generating random numbers even after a target has been found, relying on the caller to check a flag.
**Proposed Fix:** Update these templates to accept a predicate returning `bool` (stop/continue) to allow early exit.

### 5. Profiler Stack Safety
**Severity:** Low
**Location:** `src/system/kernel/scheduler/scheduler_profiler.cpp`
**Description:**
If `EnterFunction` hits the stack limit, it returns without pushing. `ExitFunction` blindly pops, potentially corrupting the stack tracking (underflow/mismatch).
**Proposed Fix:** Add logic to `ExitFunction` or `EnterFunction` to handle saturation gracefully (e.g., check depth before pop).

## Minor Issues & Observations

### 6. `sPackageToNode` Memory Management
**Severity:** Minor / Style
**Description:** `new` without `ArrayDeleter`. Manual cleanup on failure.

### 7. Stack-Based Collision Detection Limit
**Severity:** Low
**Description:** `search_global_random` limited to 1024 packages for collision detection. Acceptable tradeoff.

### 8. Mode-Specific Logic
**Status:** Verified.
`low_latency` and `power_saving` modes use consistent logic for core selection and rebalancing. `power_saving` aggressively packs small tasks.

### 9. Load Tracking Constants
**Status:** Verified.
Dynamic calculation of `kRangeReciprocal` is correct.
