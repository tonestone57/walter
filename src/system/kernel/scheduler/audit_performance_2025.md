# Haiku Scheduler Performance Bottlenecks & Optimization Plan (2025)

## 1. Identified Performance Bottlenecks

### A. Global `gTotalRunnableThreads` Contention
The global atomic counter for all runnable threads in the system is a primary bottleneck on high core-count systems due to frequent cache-line invalidations during thread state transitions.

### B. Fixed 64-CPU Scalability Limit
The use of 64-bit integers for `gIdleMask` and `gIdleNodeMask` prevents Haiku from scaling to systems with more than 64 logical processors.

### C. Context Switch Overhead from Listeners Lock
The `gSchedulerListenersLock` is a global spinlock acquired on every scheduler event, introducing serialization even when profiling or tracing is not in use.

### D. Run-Queue Lock Contention during Work-Stealing
Multiple CPUs attempting to steal from the same target CPU can lead to spinlock contention on the victim's `fQueueLock`.

---

## 2. Task List for Solution Implementation

- [ ] **Task 1: De-centralize Runnable Thread Counters**
  - Implement per-CPU runnable counts to eliminate global atomic contention.
- [ ] **Task 2: Scalable Idle Tracking**
  - Migrate `gIdleMask` and `gIdleNodeMask` to `CPUSet` to support >64 CPUs.
- [ ] **Task 3: Lock-Free Scheduler Listeners**
  - Refactor listener notifications to use RCU-safe traversal, removing global lock overhead from the context switch path.
- [ ] **Task 4: Non-Blocking Work-Stealing**
  - Optimize the stealing algorithm to favor "Try-Lock" and fast fallback over spinning on contended run-queues.
