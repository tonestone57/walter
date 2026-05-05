# Haiku Scheduler Audit & Refinement Summary (2025)

## 1. GCC 2.95 Compatibility & Portability Layer
- Established architecture-independent portability layer in `scheduler_common.h`.
- Implemented portable `scheduler_ctz`, `scheduler_popcount`, and `scheduler_ffs64` (fixed dead code and refined return values for empty masks).
- Normalized template-based `atomic_pointer_get/set/test_and_set` signatures for consistency and removed redundant `volatile` qualifiers from members.

## 2. Race Condition & Logic Fixes
- **ICI Dispatch Fix (scheduler.cpp):** Corrected a logic error where an early return skipped mandatory IPI (ICI) notifications for woken threads, resolving potential scheduling delays.
- **Authoritative RemoveCPU (scheduler_cpu.cpp):** Refined the removal process to verify the CPU's idle state via the authoritative heap key while holding the core lock.
- **Atomic Pointer Safety:** Fixed multiple TOCTOU vulnerabilities by snapshotting pointers (e.g., `fCore` in `ThreadData::GoesAway`, `fBest` in `RunQueue`).
- **Bitmask Core Indexing:** Replaced fragile counters with an atomic bitmask (`fLocalIndices`) in `CoreEntry`.

## 3. Performance & Architecture
- **3-Phase Work Stealing:** Implemented a hierarchical strategy (Sibling -> Node -> Global) to improve cache locality and reduce lock contention.
- **64-bit Atomic Alignment:** Enforced 8-byte alignment for all 64-bit variables used in atomic operations (e.g., `gIdleMask`, `fCombinedLoad`), ensuring stability on 32-bit platforms.
- **Improved RNG Entropy:** Enhanced `CPUEntry` RNG seeding with a 64-bit mixer and staggered reschedule counts.

## 4. Documentation & Maintenance
- Restored and updated all "Issue XX" fix documentation and technical commentary in source files to maintain historical context and explain complex synchronization patterns.
