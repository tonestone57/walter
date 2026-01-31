# Scheduler Audit Findings

## Critical Bugs

### 1. Work Stealing Core Mismatch
**Severity:** Critical
**Location:** `src/system/kernel/scheduler/scheduler_cpu.cpp` (`CPUEntry::ChooseNextThread`) and `src/system/kernel/scheduler/scheduler.cpp` (`reschedule`)

**Description:**
When a thread is stolen via `CPUEntry::_TryStealWork`, it is removed from the victim core's run queue but its `ThreadData::fCore` pointer is not updated to point to the new core.

**Consequences:**
1.  **Assertion Failure:** In `scheduler.cpp:reschedule`, the assertion `ASSERT(nextThreadData->Core() == core)` will fail for stolen threads, causing a kernel panic in debug builds.
2.  **Incorrect Load Tracking:** The stolen thread continues to contribute to the victim core's `fCurrentLoad` (until it sleeps/dies), while the new core processes the thread without accounting for its load. This leads to imbalanced load distribution decisions.
3.  **Potential Corruption:** `ThreadData::UnassignCore` accesses `fCore` to remove load. If the thread runs on Core A but thinks it is on Core B, it will corrupt Core B's load statistics and potentially cause race conditions if Core B is being reconfigured.

**Proposed Fix:**
In `CPUEntry::ChooseNextThread` (or `_TryStealWork`), explicitly migrate the stolen thread to the current core. This involves:
- Updating `ThreadData::fCore`.
- Updating load tracking (moving `fNeededLoad` from victim core to thief core).

## Minor Issues & Observations

### 2. `sPackageToNode` Memory Management
**Severity:** Minor / Style
**Location:** `src/system/kernel/scheduler/scheduler.cpp`

**Description:**
`sPackageToNode` is allocated using `new` without an associated `ArrayDeleter` (unlike `sCPUToCore` and `sCPUToPackage`), relying on the fact that it persists. However, if allocation fails later in `build_topology_mappings`, it is manually deleted. While correct, this inconsistency makes the code slightly more brittle.

### 3. Stack-Based Collision Detection Limit
**Severity:** Low (Documented Limitation)
**Location:** `src/system/kernel/scheduler/scheduler_topology.h`

**Description:**
`search_global_random` uses a fixed 128-byte stack bitmask to track visited packages. This limits collision detection to the first 1024 packages.
**Observation:** For systems with >1024 packages, duplicate probes may occur. This is an acceptable tradeoff for stack safety and performance.

### 4. Load Tracking Constants
**Severity:** Info
**Location:** `src/system/kernel/scheduler/scheduler_thread.cpp`

**Description:**
`kRangeReciprocal` is calculated dynamically in `ComputeQuantum`, ensuring correctness even if `kMaxLoad` changes. The calculation `1311` matches the standard 1000/200 load range.

## Logic Review

### Locking Strategy
**Analysis:**
- `ChooseNextThread` holds the local Core lock.
- `_TryStealWork` attempts to acquire victim Core locks.
- **Safety:** It uses `TryLockRunQueue` for victim cores. This prevents deadlocks where two cores try to steal from each other simultaneously.

### Virtual Deadline
**Analysis:**
- The mapping from `fVirtualDeadline` to `fEffectivePriority` (0-99) is linear and clamped.
- `sVirtualDeadlineSlices` pre-computation avoids expensive division in the hot path.
