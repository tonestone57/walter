# Haiku Scheduler Performance Bottlenecks & Optimization Plan (2025)

## 1. Core Scalability Features (Implemented)

*   **Idle-Only Stealing:** Cores only rebalance when work is exhausted, preserving throughput for busy cores.
*   **Lock-Free Bit-Stealing:** Uses atomic bitmask manipulation to reduce spinlock contention during work-stealing.
*   **Topology Awareness:** Tiered stealing (LLC -> NUMA -> Global) with lag-threshold gating to preserve cache warmth.

## 2. Identified Performance Bottlenecks (Pending)

### A. Global `gTotalRunnableThreads` Contention
Global atomic counter for all runnable threads causes high cache-line contention on large systems.

### B. Fixed 64-CPU Scalability Limit
`gIdleMask` and `gIdleNodeMask` (uint64) prevent scaling beyond 64 logical processors or NUMA nodes.

### C. Context Switch Overhead from Listeners Lock
`gSchedulerListenersLock` (global spinlock) is acquired on every scheduler event.

### D. Run-Queue Lock Contention during Work-Stealing
Multiple concurrent steal attempts can still contend on a single victim's `fQueueLock`.

---

## 3. Task List for Solution Implementation

- [ ] **Task 1: De-centralize Runnable Thread Counters**
- [ ] **Task 2: Scalable Idle Tracking (Migrate to `CPUSet`)**
- [ ] **Task 3: Lock-Free Scheduler Listeners (RCU)**
- [ ] **Task 4: Non-Blocking Work-Stealing Refinement**
