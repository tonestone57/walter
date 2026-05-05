# Haiku Scheduler Full Code Audit & Fixes (2025)

## 1. Overview
A comprehensive code audit of the Haiku scheduler (`src/system/kernel/scheduler`) was performed to ensure architecture independence, GCC 2.95 compatibility, and overall logic correctness for both 32-bit and 64-bit systems.

## 2. Key Robustness Fixes Implemented

### A. Core-Local Index Management
- **Issue:** The previous counter-based `fNextCoreLocalIndex` was susceptible to drift and duplication during CPU hot-unplug events, leading to incorrect round-robin ownership in `UpdatePriorityBoostScalable`.
- **Fix:** Replaced the counter with a robust bitmask-based allocation (`fLocalIndices`) in `CoreEntry`.
- **Mechanism:** `AddCPU` now reserves the first available bit using an atomic CAS loop. `RemoveCPU` atomically clears the bit.
- **Density Guarantee:** `UpdatePriorityBoostScalable` in `scheduler.cpp` was updated to derive a dense round-robin index using `scheduler_popcount` on the bitmask, ensuring fairness even when the index range has "holes" after removals.

### B. Accurate Idle Transitions
- **Issue:** Potential race conditions in `RemoveCPU` when updating `fIdleCPUCount` and transitioning cores to idle state.
- **Fix:** Simplified `RemoveCPU` to use the authoritative heap key check while holding `fCPULock`. Correctly handles `fPackage->CoreGoesIdle` when the last busy CPU of a core is removed.

### C. Architecture-Independent Portability Helpers
- **Helpers Added:** Introduced `scheduler_atomic_test_and_set`, `scheduler_atomic_and`, `scheduler_atomic_or`, and `scheduler_atomic_get` for `native_cpu_mask_t`.
- **Bitwise Helpers:** Implemented portable `scheduler_ctz`, `scheduler_ffs64`, and `scheduler_popcount` to replace non-standard GCC built-ins.
- **64-bit Safety:** Verified and enforced 8-byte alignment for all 64-bit atomic variables (e.g., `fVirtualRuntime`) for safety on 32-bit platforms.

### D. GCC 2.95 Compatibility
- **Atomic Pointers:** Updated `atomic_pointer_get` calls with explicit template arguments and `const volatile` casts to satisfy legacy compiler requirements and ensure visibility across cores.
- **Namespace Scope:** Verified that all types used in template instantiations are defined at namespace scope.

## 3. Audited & Corrected Core Logic
The audit confirmed and refined the following critical logic sections, ensuring they are correctly captured in the patch:
- **Work Stealing:** `StealThread` correctly uses `PeekOption` with affinity predicates and proper rollback on failure.
- **RNG Entropy:** Improved per-CPU RNG initialization with mixed entropy sources (system time, ASLR addresses).
- **Reschedule ICI:** Fixed a race in `scheduler.cpp::enqueue` where an early return made IPI dispatch unreachable, causing indefinite scheduling delays.
- **Load Average:** Corrected the fixed-point load average calculation in `scheduler_load.cpp` to prevent 64-bit overflows and correctly handle 5-second tick calibration.
- **NUMA Affinity:** Enhanced `choose_core` and `rebalance` in `low_latency.cpp` and `power_saving.cpp` with robust NULL guards and optimized affinity-aware searching.

## 4. Conclusion
The Haiku scheduler is now robust against dynamic hardware changes and maintains full compatibility across legacy and modern toolchains on all supported architectures.
