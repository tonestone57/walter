# Haiku Scheduler Audit & Optimization Report (2025)

## 1. Architectural Foundation (Prior Phases)

### GCC 13 Compatibility & Standardized Atomics
- Established a robust set of atomic helpers in `scheduler_common.h` (`scheduler_atomic_*64`, `LoadAcquire`, `StoreRelease`, etc.) using explicit casts to satisfy GCC 13's stricter type checking.
- Implemented type-safe wrappers for `int32` atomic operations across the scheduler subsystem.

### 32-bit & 64-bit Portability
- Enforced 8-byte alignment for all 64-bit variables accessed via atomic operations.
- Utilized `native_cpu_mask_t` and portable bit manipulation intrinsics (`scheduler_ctz`, `scheduler_ffs64`, `scheduler_popcount`) to handle varying CPU mask widths.

### Hybrid RunQueue Structure
- Refactored `ThreadRunQueue` to a hybrid data structure combining dual array-based binary min-heaps for EEVDF deadline sorting with per-priority circular doubly-linked lists and a priority bitmap. This enables $O(1)$ retrieval by priority while maintaining $O(\log N)$ heap complexity for deadline-based scheduling.

### Phase 2 Scalability & Cache Efficiency
- **De-centralized RCU Management**: Moved RCU callback queues and spinlocks from global scope into the `CPUEntry` class.
- **Global Interaction State Alignment**: Grouped interactivity variables into a 64-byte aligned `InteractivityState` structure to eliminate false sharing.
- **Hierarchical Work-Stealing Sampling**: Refactored `search_global_random` to utilize a Node -> Package sampling strategy for improved NUMA coverage.

## 2. Phase 3 Scalability Improvements (Implemented)

### A. De-centralized Runnable Thread Accounting
- **Status**: **Resolved**.
- **Problem**: Global `gTotalRunnableThreads` atomic counter caused high cache-line contention on many-core systems.
- **Solution**: Replaced with per-CPU `fRunnableCount` tracking. Symmetric accounting is maintained via `ThreadData::fAssignedCPU`, ensuring count integrity across context switches and migrations.

### B. Scalable CPUSet-based Idle Tracking
- **Status**: **Resolved**.
- **Problem**: Fixed 64-bit `uint64` masks strictly limited scalability to 64 CPUs.
- **Solution**: Migrated `gIdleMask` and `gIdleNodeMask` to the `CPUSet` class, removing the hard-coded ceiling and enabling support for systems with arbitrary CPU counts.

### C. Refactored Mask Iteration
- **Status**: **Resolved**.
- **Implementation**: Updated all search and rebalancing loops in `low_latency.cpp` and `power_saving.cpp` to use scalable bitset iteration via `CPUSet::Bits(i)`.

### D. Optimized Rebalancing Logic
- **Status**: **Resolved**.
- **Features**: Implemented idle-only stealing to ensure busy cores are never interrupted for rebalancing, maximizing throughput. Enhanced topology awareness for LLC and NUMA-aware work-stealing to preserve data locality.

### E. Atomic Scheduler Snapshots
- **Status**: **Resolved**.
- **Problem**: `MakeSchedulerSnapshot` could perform "torn" reads of the idle mask on 32-bit or high-core-count systems.
- **Solution**: Refactored to use word-by-word `LoadAcquire` scanning, providing a consistent and near-atomic view of system state for load-balancing decisions.

### F. Synchronization & DPC Refinements
- **Status**: **Resolved**.
- **Features**: Optimized `update_quantum_lengths_dpc` with an atomic re-check loop to prevent lost resolution updates. Eliminated redundant `scheduler_synchronize` calls in the interactivity hot-path. Integrated RCU-callback processing into the periodic interactivity DPC to ensure timely cleanup without global lock contention.

## 3. Pending Performance Bottlenecks (Phase 4 Roadmap)

### A. Global Listeners Lock (`gSchedulerListenersLock`)
- **Status**: **Resolved**.
- **Problem**: Global read-write spinlock was acquired on every scheduler event.
- **Solution**: Migrated scheduler listeners to an RCU-safe lock-free singly-linked list. `NotifySchedulerListeners` now performs lock-free traversal, significantly reducing contention on many-core systems.

### B. Static Work-Stealing Thresholds
- **Problem**: Current lag thresholds (1ms/2ms/5ms) are hard-coded. On high-bandwidth or extremely low-latency interconnects, these might be too conservative.
- **Solution**: Adjust lag thresholds dynamically based on interconnect bandwidth and latency detected at boot.

### C. Heterogeneous Load Balancing Overhead
- **Problem**: Modern P/E core architectures require more frequent but low-overhead load updates to stay within the "efficiency sweet spot."
- **Solution**: Implement Hardware-Guided EAS (Energy-Aware Scheduling).

### D. DPC Queue Scaling
- **Status**: **Resolved**.
- **Problem**: Global DPC queues (`sNormalPriorityQueue`, etc.) caused serialization and cache-line bouncing on many-core systems.
- **Solution**: Implemented per-CPU DPC queues. Each CPU now maintains its own set of DPC threads pinned to that CPU. The scheduler utilizes `DPCQueue::CPUQueue` to dispatch RCU callback processing and IRQ rebalancing to specific CPUs, ensuring strict cache locality and eliminating global lock contention in the DPC subsystem.

## 4. Phase 4 Roadmap Task List

- [x] **Task 1: Implement RCU-safe Scheduler Listeners**
- [x] **Task 2: Dynamic Interconnect-Aware Thresholds**
- [ ] **Task 3: Hardware-Guided EAS (Energy-Aware Scheduling)**
- [x] **Task 4: Per-CPU DPC Queue Auditing & Optimization**
- [x] **Task 5: Optimized Runnable Thread Accounting**
- [x] **Task 6: Core-Local Topology Iteration Optimization**
- [x] **Task 7: Layered Flat Bitmask (LFB) for O(1) Idle Discovery**
- [x] **Task 8: Directed Quantum Handoff (DQH) for Message Chains**
- [x] **Task 9: Extensive 2025 Final Audit & Bug Squash**
- [x] **Task 10: Final Synchronization Audit & LFB Stability**

## 5. Performance Bottlenecks & Future Scalability

### A. CPUSet Iteration Overhead
On systems with 128+ cores, functions like `CoreEntry::GetMinVirtualRuntime` and `CheckMaskedPackagesMinimumLoad` perform linear scans of `CPUSet` bitmasks. While optimized with `scheduler_ctz`, the $O(N)$ complexity becomes a measurable bottleneck during high-frequency scheduling events.
*   **Mitigation**:
    - **Decentralized Runnable Counting**: Optimized global thread accounting via decentralized per-node summation ($O(Nodes)$) instead of a single global atomic counter to prevent cache-line contention.
    - **Core-Local Bitmasks**: Refactored `CoreEntry` to utilize 64-bit `fLocalIndices` for core-local searches, reducing the scan scope from the global `CPUSet` (SMP_MAX_CPUS) to the physical core (max 64 lanes).
    - **Layered Flat Bitmask (LFB)**: Implemented a two-tiered atomic bitmask structure (`gIdleNodeSummary` and `gIdleCoresInNode`) for deterministic $O(1) discovery of idle cores across up to 4096 cores, eliminating hierarchical tree walks.

### B. Random Sampling Latency
`search_global_random` in `scheduler_topology.h` utilizes a hierarchical Node -> Package sampling strategy. On many-node NUMA systems, the multiple levels of RNG calls and bitmask deduplication introduce latency.
*   **Mitigation**: Optimize RNG paths and use thread-local sampling buffers.

### C. Power Saving Mode Contention
The `sSmallTaskCore` array in `power_saving.cpp` is a source of cache-line contention for CPUs within the same Node/L3 domain. Frequent updates to the consolidation target can lead to "ping-ponging" of cache lines between sibling cores.
*   **Mitigation**: Pad `sSmallTaskCore` entries to cache-line boundaries or use per-cluster consolidation hints.

### D. EEVDF Matrix Resolution Updates
Updating the EEVDF matrix resolution (via `update_quantum_lengths_dpc`) requires a global ICI broadcast and RCU synchronization. Frequent interactivity changes (e.g., rapid window switching) can trigger these expensive operations too often.
*   **Mitigation**: Implemented a 50ms cooldown "dampening" filter and an atomic re-check loop in the resolution update DPC to consolidate rapid changes.

### E. Message-Driven UI Latency
Coupled threads (e.g., `app_server` and UI looper) frequently block on each other, leading to repeated "GoesAway/Enqueue" cycles that incur full selection matrix search costs.
*   **Mitigation**: Implemented **Directed Quantum Handoff (DQH)**. Interacting threads can now donate their remaining timeslice and trigger an immediate context switch to the partner thread, bypassing the run-queue selection logic entirely for message chains.

## 6. 2025 Extensive Audit Findings & Fixes

During the final extensive audit in 2025, several critical bugs and logic errors were identified and resolved:

### A. 32-bit Memory Corruption in RunQueue
- **Problem**: `RunQueue::Remove` utilized a raw `AtomicAnd64` on a `native_cpu_mask_t` field. On 32-bit systems, where the mask is only 32 bits, this caused a 4-byte out-of-bounds write, corrupting adjacent scheduler metadata.
- **Solution**: Replaced raw atomic calls with the architecture-aware `cpu_mask_and_atomic` helper.

### B. Fixed-Point Urgency Logic Error
- **Problem**: `ThreadData::_ComputeEffectivePriority` failed to handle overdue threads (`diff <= 0`) before entering the fixed-point math block. This caused unsigned overflow/wrap, resulting in overdue threads receiving minimum instead of maximum urgency.
- **Solution**: Added an explicit check for overdue threads to assign maximum dynamic priority immediately.

### C. Atomic Standardization
- **Problem**: Inconsistent use of raw `atomic_get64` versus standardized `LoadAcquire64` wrappers across different modules.
- **Solution**: Standardized all atomic accesses in the hot path to use the `LoadAcquire`/`StoreRelease` wrappers, ensuring proper memory barriers on weakly-ordered architectures.

### D. Deadlock & Livelock Mitigation
- **Problem**: Identified potential for kernel deadlock in `DonateTimesliceTo` when threads block in a circular chain. Also found several unbounded `while(true)` CAS loops that could livelock under extreme contention.
- **Solution**: Replaced blocking locks with bounded try-locking in donation paths. Added retry limits and `cpu_pause()` to all critical CAS loops to ensure system progress.

### E. LFB Summary Stability
- **Problem**: A race condition in `UpdateIdleCoreLFB` could lead to a permanent loss of the node-idle summary bit if a core became idle exactly when the bit was being cleared by another CPU.
- **Solution**: Added a post-CAS race check to re-verify node idleness and restore the summary bit if needed.
