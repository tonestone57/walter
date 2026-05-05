# Haiku Scheduler Full Code Audit & Fixes (2025)

## 1. Overview
A comprehensive code audit of the Haiku scheduler (`src/system/kernel/scheduler`) was performed to ensure architecture independence, GCC 2.95 compatibility, and overall logic correctness for both 32-bit and 64-bit systems.

## 2. New Fixes Implemented

### A. Robust Core-Local Index Management
- **Issue:** The previous counter-based `fNextCoreLocalIndex` was susceptible to drift and duplication during CPU hot-unplug events, leading to incorrect round-robin ownership in `UpdatePriorityBoostScalable`.
- **Fix:** Replaced the counter with a bitmask (`fLocalIndices`) in `CoreEntry`.
- **Mechanism:** `AddCPU` now reserves the first available bit using a CAS loop. `RemoveCPU` atomically clears the bit. This guarantees unique, dense, and reusable indices within `[0, CPUCount)`.

### B. Accurate Idle Transitions in `RemoveCPU`
- **Issue:** Potential TOCTOU race in `RemoveCPU` when updating `fIdleCPUCount`.
- **Fix:** Since `RemoveCPU` is called under `fCPULock`, the idle state of the CPU being removed is stable. Replaced the complex retry loop with an authoritative check of the CPU's heap key.
- **Improved Logic:** If the core was busy but becomes fully idle after removing the last busy CPU, `fPackage->CoreGoesIdle` is now correctly called.

### C. Architecture Independence & Atomic Safety
- **Helper Added:** `scheduler_atomic_test_and_set` in `scheduler_common.h` provides a portable CAS wrapper for `native_cpu_mask_t`.
- **Bitwise Helpers:** Added `scheduler_ctz` and `scheduler_ffs64` portable wrappers to `scheduler_common.h` to replace GCC built-ins not supported in 2.95.
- **Alignment:** Verified all 64-bit atomic variables (e.g., `fVirtualRuntime`, `gDeadlineBucketSize`) use `__attribute__((aligned(8)))`.

### D. GCC 2.95 Compatibility
- **Explicit Arguments:** Added explicit template arguments to `atomic_pointer_get` calls in `scheduler_profiler.cpp` with `const volatile` casts to satisfy older compiler requirements and ensure visibility.

## 3. Audited & Confirmed Existing Fixes
The following previously implemented fixes were audited and confirmed to be correct in the current logic:
- **Work Stealing Logic:** Confirmed `StealThread` uses `PeekOption` with affinity predicates.
- **RNG Contention:** Verified per-CPU RNG usage in topology-aware search functions.
- **Reschedule ICI:** Confirmed IPI dispatch logic in `scheduler.cpp` handles wakeup of target CPUs correctly.
- **Load Scaling:** Verified quantum scaling constants use optimized reciprocal-based arithmetic.
- **NUMA Awareness:** Confirmed NULL guards for `Package()` and `Node()` are present in all critical paths.

## 4. Conclusion
The Haiku scheduler is now robust against CPU topology changes and maintains full compatibility with legacy and modern toolchains across all supported architectures.
