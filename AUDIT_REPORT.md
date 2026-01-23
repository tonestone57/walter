# Scheduler Code Audit Report

## Overview
A full code analysis and audit of `src/system/kernel/scheduler` was performed to ensure accuracy and soundness of the logic.

## Summary
The scheduler implementation is **sound and accurate**. The architecture successfully balances low-latency requirements with scalability and heterogeneous hardware support.

## Key Findings

### 1. Data Structures & Scalability
- **RunQueue**: The bitmap-based priority queue implementation correctly uses O(1) operations (or close to it with `fls`) to find the highest priority thread. The `PeekBest` optimization with dynamic search depth balances strict priority with fairness.
- **Hierarchical Bitmasks**: `SchedulerNode`, `PackageEntry`, and `CoreEntry` correctly implement a hierarchical bitmask system for tracking idle resources. This allows O(1) discovery of idle cores across large systems (up to 4096 packages).
- **Scalable Priority Boosting**: The `UpdatePriorityBoostScalable` function efficiently scans only the heads of priority queues, avoiding O(N) iteration over all threads.

### 2. Low Latency Mode
- **Core Selection**: `choose_core` in `low_latency.cpp` implements a robust search strategy:
    1. Cache Affinity (Previous Core, Sibling Cores).
    2. NUMA/Cluster Affinity (Local Node).
    3. Home Node Affinity.
    4. Global Idle Search (Hierarchical).
    5. Global Load Balancing (Random Sampling).
- **Random Sampling**: The "Power of Two Choices" variant (sampling 16 packages) used in `choose_core` and `rebalance` ensures scalability on large systems by avoiding global locks and linear scans.
- **Fallback**: The fallback to linear scan ensures a core is always found even if random sampling fails, preventing starvation.

### 3. Power Saving Mode
- **Packing Strategy**: The mode correctly prioritizes packing threads onto fewer cores/packages to allow others to idle.
- **Scalability Trade-off**: The `rebalance_irqs` and `choose_core` functions in `power_saving.cpp` utilize O(N) linear scans over packages. This is a deliberate design choice to maximize packing quality over latency, suitable for the intended use case (power saving on typically smaller devices), though it may limit scalability on massive servers compared to `low_latency` mode.

### 4. Logic Verification
- **Bitwise Operations**: The complex bitwise logic in `CPUEntry::_TryStealWork` (mask inversion for busy package finding) and `RunQueue` iterators was verified via a separate simulation script and found to be correct.
- **Memory Safety**: Use of `ArrayDeleter` and proper locking (`spinlock`, `rw_spinlock`) ensures memory safety and concurrency correctness.
- **Topology Handling**: The topology building logic correctly handles heterogeneous CPUs (calculating capacity) and clamps massive topologies (splitting large packages) to fit internal limits safely.

### 5. Potential Edge Cases (Verified)
- **Affinity Masks**: The interaction between CPU affinity masks and core selection logic is handled correctly. If a mask effectively disables all CPUs, the scheduler relies on `gCPUEnabled` checks to prevent invalid selections.
- **Quantum Calculation**: The integer arithmetic in `ThreadData::ComputeQuantum` for load scaling was verified and is accurate.

## Conclusion
The scheduler codebase is high-quality, implementing advanced features like topology awareness and heterogeneous support correctly. No critical bugs or logical flaws were identified.
