# Haiku Scheduler Audit & Refinement Summary (2025)

## 1. GCC 2.95 Compatibility & Portability Layer
- Established architecture-independent portability layer in \`scheduler_common.h\`.
- Implemented portable \`scheduler_ctz\`, \`scheduler_popcount\`, and \`scheduler_ffs64\`.
- Normalized template-based \`atomic_pointer_get/set/test_and_set\` signatures for consistency and removed redundant \`volatile\` qualifiers from members.

## 2. Race Condition & Logic Fixes
- **ICI Dispatch Fix (scheduler.cpp):** Corrected a logic error where an early return skipped mandatory IPI (ICI) notifications for woken threads, resolving potential scheduling delays.
- **Authoritative RemoveCPU (scheduler_cpu.cpp):** Refined the removal process to verify the CPU's idle state via the authoritative heap key while holding the core lock.
- **Atomic Pointer Safety:** Fixed multiple TOCTOU vulnerabilities by snapshotting pointers (e.g., \`fCore\` in \`ThreadData::GoesAway\`, \`fBest\` in \`RunQueue\`).
- **Bitmask Core Indexing:** Replaced fragile counters with an atomic bitmask (\`fLocalIndices\`) in \`CoreEntry\`.

## 3. Performance & Architecture
- **Timestamp Propagation:** Capture \`system_time()\` once at the start of the \`reschedule()\` hot path and propagate it through the call stack (accounting, load tracking, quantum calculation). This eliminates redundant hardware timer reads in the kernel's most frequent execution path.
- **3-Phase Work Stealing:** Implemented a hierarchical strategy (Sibling -> Node -> Global) to improve cache locality and reduce lock contention.
- **64-bit Atomic Alignment:** Enforced 8-byte alignment for all 64-bit variables used in atomic operations (e.g., \`gIdleMask\`, \`fCombinedLoad\`), ensuring stability on 32-bit platforms.
- **Improved RNG Entropy:** Enhanced \`CPUEntry\` RNG seeding with a 64-bit mixer and staggered reschedule counts.

## 4. Concurrency & Safety Refinements
- **Lock Hierarchy Standardization:** Established and enforced a strict Core RunQueue -> CPU RunQueue locking order to prevent potential deadlocks.
- **Thread Selection Race Fix:** Hardened \`CPUEntry::ChooseNextThread\` to ensure peeked threads are removed from queues while holding necessary locks, preventing they being stolen between peeking and removal.
- **Hot-Unplug Hardening:** Added explicit \`NULL\` guards for topology dereferences (\`Package()\`, \`Node()\`) in core-selection and rebalancing routines to prevent kernel panics during CPU hot-unplug events.
- **Priority Boost Correctness:** Fixed a regression where effective priority was not recalculated after a boost reset, ensuring immediate impact on scheduling decisions.
- **Enhanced Serialization (Issue 89):** Added explicit serialization via \`fCoreLock\` to \`CoreGoesIdle\` and \`CoreWakesUp\` to prevent races during package state transitions.

## 5. 2025 Audit Improvements
- **Sub-second Load Visibility (Issue 16):** Increased load average resolution from 5 seconds to 1 second and recalibrated EMA decay constants to improve responsiveness for interactive workloads.
- **IRQ Draining Refinement (Issue 15):** Refined the IRQ draining logic in \`CPUEntry::Stop\` with progress verification and enhanced documentation, ensuring all vectors are reassigned without risking infinite loops.
- **Lock Ordering Verification (Issue 91):** Formally verified the \`scheduler_lock\` vs. \`fCPULock\` hierarchy and documented the lack of hazards in the current implementation.

## 6. Documentation & Maintenance
- Restored and updated all "Issue XX" fix documentation and technical commentary in source files to maintain historical context and explain complex synchronization patterns.
- Addressed documented future improvements by completing timestamp propagation (Issue 72), improved package state serialization (Issue 89), and refined IRQ draining (Issue 15).
