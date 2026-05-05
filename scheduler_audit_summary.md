# Haiku Scheduler Audit & Portability Fixes (2025)

## 1. Portability Layer (scheduler_common.h)
- Implemented `native_cpu_mask_t` (32/64-bit) and `scheduler_atomic_*` wrappers.
- Added portable `scheduler_ctz`, `scheduler_popcount`, and `scheduler_ffs64` for GCC 2.95 compatibility.
- Implemented template-based `atomic_pointer_get/set/test_and_set` to standardize pointer atomicity.

## 2. Race Condition & Logic Fixes
- **ICI Dispatch Fix (scheduler.cpp):** Corrected a logic error where an early return skipped mandatory IPI (ICI) notifications for woken threads, resolving potential scheduling delays.
- **Authoritative RemoveCPU (scheduler_cpu.cpp):** Refined the removal process to verify the CPU's idle state via the authoritative heap key while holding the core lock, resolving Issue 96.
- **Atomic Pointer Safety:** Fixed multiple TOCTOU vulnerabilities by snapshotting pointers (e.g., `fCore` in `ThreadData::GoesAway`, `fBest` in `RunQueue`).
- **Bitmask Core Indexing:** Replaced fragile counters with an atomic bitmask (`fLocalIndices`) in `CoreEntry`, ensuring safe concurrent CPU activation.

## 3. Performance & Architecture
- **3-Phase Work Stealing:** Implemented a hierarchical strategy (Sibling -> Node -> Global) to improve cache locality and reduce lock contention.
- **64-bit Atomic Alignment:** Enforced 8-byte alignment for all 64-bit variables used in atomic operations (e.g., `gIdleMask`, `fCombinedLoad`), ensuring stability on 32-bit platforms.
- **Improved RNG Entropy:** Enhanced `CPUEntry` RNG seeding with a 64-bit mixer and staggered reschedule counts to minimize lock spikes.

## 4. Documentation & Cleanup
- Restored and updated all "Issue XX" fix documentation and technical commentary in source files to maintain maintainability.
- Synchronized `atomic_pointer_get` signature and removed redundant `volatile` qualifiers where atomic helpers provide sufficient visibility.
