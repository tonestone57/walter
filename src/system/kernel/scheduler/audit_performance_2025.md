# Haiku Scheduler Performance Audit & Optimization Report (2025)

## 1. Executive Summary
A comprehensive audit of the Haiku kernel scheduler was performed, focusing on scalability and cache efficiency on multi-core systems. Several critical bottlenecks were identified in the areas of RCU management, global state synchronization, and work-stealing topology. These bottlenecks were addressed through de-centralization and alignment optimizations.

## 2. Identified & Resolved Bottlenecks

### A. Global RCU Callback Contention (Resolved)
- **Problem**: RCU callback management used a single global list and spinlock, causing serialization on many-core systems.
- **Solution**: De-centralized RCU management by moving callback queues and locks into the `CPUEntry` class. Each CPU now independently manages its grace-period callbacks independently.
- **Impact**: Significant reduction in lock contention during thread destruction and interaction state updates.

### B. Global Interaction State False Sharing (Resolved)
- **Problem**: Global variables `sLastInteractionTime`, `sDPCPending`, `sTimerArmed`, and `sPendingDPCTarget` were individual globals, likely residing on the same cache line.
- **Solution**: Encapsulated these variables into a 64-byte aligned `InteractivityState` structure.
- **Impact**: Eliminated cache-line bouncing (false sharing) between CPUs frequently updating their interactivity scores and quantum targets.

### C. Inefficient Work-Stealing Sampling (Resolved)
- **Problem**: `search_global_random` used a flat random sampling of packages, which ignored NUMA node boundaries and led to sub-optimal probe distributions on large systems.
- **Solution**: Implemented a hierarchical (Node -> Package) sampling strategy in `scheduler_topology.h`.
- **Impact**: Ensures even coverage across all NUMA domains and improved cache locality for stealing probes.

### D. Redundant Static Definitions (Resolved)
- **Problem**: `TakeSnapshot` was redefined in multiple units, and `rcu_callback` visibility was limited.
- **Solution**: Consolidated `TakeSnapshot` as an inline helper in `scheduler_common.h`.

---

## 3. Implemented Task List

- [x] **Task 1: De-centralize RCU Management**
  - Moved `sPendingCallbacks` and `sRCUCallbackLock` to `CPUEntry`.
  - Updated `scheduler_call_rcu` and `scheduler_process_rcu_callbacks`.
- [x] **Task 2: Interaction State Grouping & Alignment**
  - Created `InteractivityState` struct with `CACHE_LINE_ALIGN`.
  - Refactored all global interaction variables in `scheduler.cpp`.
- [x] **Task 3: Hierarchical Work-Stealing Sampling**
  - Refined `search_global_random` in `scheduler_topology.h` to use Node-aware sampling.
- [x] **Task 4: Structural Consolidation**
  - Moved `TakeSnapshot` and `rcu_callback` to `scheduler_common.h`.
