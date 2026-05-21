# Haiku Scheduler Audit & Optimization Report (2025 - Phase 3)

## 1. Executive Summary
An extensive audit of the Haiku kernel scheduler was performed to identify remaining performance bottlenecks and scalability limits. While previous phases successfully decentralized RCU management and improved cache efficiency, several global synchronization points and hard-coded capacity limits still restrict scalability on systems with high core counts (64+).

## 2. Core Scalability Achievements (Phase 1 & 2)

### A. Idle-Only Work Stealing
- **Status**: **Resolved**.
- **Implementation**: Cores only attempt to steal work (`ChooseNextThread`) when their local run-queue is empty. This prevents busy cores from wasting cycles on rebalancing, maximizing raw compute throughput.

### B. Lock-Free Bit-Stealing
- **Status**: **Resolved**.
- **Implementation**: Thief CPUs use atomic `TestAndClear` operations on a victim's run-queue bitmaps before attempting to acquire the `fQueueLock`. This significantly reduces spinlock contention and cache-line bouncing.

### C. Cache-Domain & NUMA Awareness
- **Status**: **Resolved**.
- **Implementation**: Tiered work-stealing (L3 -> NUMA -> Global) gated by increasing lag thresholds (`kL3LagThreshold` to `kGlobalLagThreshold`). Interactive threads are gated against cross-socket migration to preserve cache warmth and responsiveness.

---

## 3. Identified Performance Bottlenecks & Global Spinlocks

### A. Global Runnable Counter Contention (`gTotalRunnableThreads`)
- **Problem**: Every thread enqueue, dequeue, and migration performs atomic operations on this single 32-bit global counter. On many-core systems, this becomes a major point of cache line bouncing and contention.
- **Impact**: Significant overhead in the scheduling hot-path during high thread churn.

### B. Fixed-Width Idle Masks (`gIdleMask` & `gIdleNodeMask`)
- **Problem**: These masks are implemented as `uint64`, strictly limiting Haiku's ability to track idle states for more than 64 CPUs or 64 NUMA nodes.
- **Impact**: Hard scalability ceiling for massive systems.

### C. Global Listeners Lock (`gSchedulerListenersLock`)
- **Problem**: A global read-write spinlock is acquired during every enqueue and schedule event to protect the scheduler listeners list.
- **Impact**: Adds unnecessary overhead to every context switch, even when no listeners are active.

### D. Work-Stealing Lock Contention (`fQueueLock`)
- **Problem**: While run-queues are decentralized, a "victim" CPU's `fQueueLock` can still experience high contention if multiple "thief" CPUs attempt to steal work from it simultaneously.
- **Impact**: Increased latency for the victim CPU's local rescheduling.

---

## 4. Pending Tasks & Proposed Solutions

- [ ] **Task 1: De-centralize Runnable Thread Accounting**
  - Replace `gTotalRunnableThreads` with per-CPU or per-Node counters.
  - Implement a "fuzzy" or cached global sum for load-balancing decisions.

- [ ] **Task 2: Implement Scalable CPUSet-based Idle Tracking**
  - Refactor `gIdleMask` and `gIdleNodeMask` to use the `CPUSet` class to support >64 CPUs.

- [ ] **Task 3: Implement RCU-Protected Scheduler Listeners**
  - Convert `gSchedulerListeners` to an RCU-safe list and remove `gSchedulerListenersLock` from the hot-path.

- [ ] **Task 4: Optimize Work-Stealing with Non-Blocking TryLock**
  - Refine `StealThreadLockless` to favor non-blocking `TryLockRunQueue` for all stealing attempts.
