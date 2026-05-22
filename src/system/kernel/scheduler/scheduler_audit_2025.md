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
- **Problem**: Potential serialization in global DPC processing.
- **Solution**: Perform a per-CPU DPC queue auditing and optimization.

## 4. Phase 4 Roadmap Task List

- [x] **Task 1: Implement RCU-safe Scheduler Listeners**
- [ ] **Task 2: Dynamic Interconnect-Aware Thresholds**
- [ ] **Task 3: Hardware-Guided EAS (Energy-Aware Scheduling)**
- [ ] **Task 4: Per-CPU DPC Queue Auditing & Optimization**
