# Haiku Scheduler Audit & Optimization Report (2025 - Phase 3)

## 1. Executive Summary
An extensive audit of the Haiku kernel scheduler was performed to identify remaining performance bottlenecks and scalability limits. While previous phases successfully decentralized RCU management and improved cache efficiency, several global synchronization points and hard-coded capacity limits still restrict scalability on systems with high core counts (64+).

## 2. Identified Performance Bottlenecks & Global Spinlocks

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

## 3. Pending Tasks & Proposed Solutions

- [ ] **Task 1: De-centralize Runnable Thread Accounting**
  - Replace `gTotalRunnableThreads` with per-CPU or per-Node counters.
  - Modify `ThreadData::Enqueue` and `Dequeue` to update local counters.
  - Implement a "fuzzy" or cached global sum for load-balancing decisions to avoid frequent global synchronizations.

- [ ] **Task 2: Implement Scalable CPUSet-based Idle Tracking**
  - Refactor `gIdleMask` and `gIdleNodeMask` to use the `CPUSet` class or a dynamically sized bitset.
  - Update `SetCPUIDle`, `ClearCPUIDle`, and `IsCPUIDle` helpers to support arbitrary bit widths.

- [ ] **Task 3: Implement RCU-Protected Scheduler Listeners**
  - Convert `gSchedulerListeners` to an RCU-safe list.
  - Remove `gSchedulerListenersLock` from the notification path (`NotifySchedulerListeners`).
  - Use `scheduler_call_rcu` for listener removal to ensure safe grace periods.

- [ ] **Task 4: Optimize Work-Stealing with Non-Blocking TryLock**
  - Refine `StealThreadLockless` to use a non-blocking `TryLockRunQueue` approach that immediately skips to the next candidate if contention is detected.
  - Reduce the "thief" priority if it repeatedly fails to acquire a lock to prevent livelock.
