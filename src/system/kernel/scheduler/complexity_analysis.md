# Haiku Scheduler Complexity Analysis

This document provides a breakdown of the time complexity for core functions within the Haiku BMQ-EEVDF scheduler.

## 1. RunQueue Operations (src/system/kernel/scheduler/RunQueue.h)

### `PeekBest()`
- **Complexity:** $O(1)$
- **Details:** The function performs a constant-time check on the Real-Time bitmap (32 bits) and then scans a fixed number of words in the FairShare bitmap (512 lanes = 8 or 16 words). Since the number of bins is fixed, the execution time does not depend on the number of threads in the system.

### `PushBack()` / `PushFront()` / `Remove()`
- **Complexity:** $O(1)$
- **Details:** These operations perform basic arithmetic for bin calculation and doubly-linked list manipulation. Bitmap updates are atomic bitwise operations.

---

## 2. Thread Placement & Balancing (src/system/kernel/scheduler/low_latency.cpp & power_saving.cpp)

### `choose_core()`
- **Complexity:** $O(1)$ to $O(N)$
- **Best Case ($O(1)$):** Hits the "Hot-Idle" fast path or finds a suitable core through hierarchical random sampling (fixed number of probes).
- **Average Case ($O(S)$):** Probes $S$ random packages (where $S$ is between 16 and 64, calibrated at boot).
- **Worst Case ($O(N)$):** When a specific CPU affinity mask is provided, the scheduler may perform a linear scan of the mask to find eligible packages.

### `rebalance()`
- **Complexity:** $O(1)$ to $O(N)$
- **Details:** Similar to `choose_core`. It uses random sampling to find a "better" core. The complexity is bounded by the number of probes ($O(S)$) unless an affinity mask forces a linear scan.

---

## 3. Scheduling Hot Path (src/system/kernel/scheduler/scheduler.cpp)

### `reschedule()`
- **Complexity:** $O(1)$
- **Details:** Calls `ChooseNextThread` and `TrackLoad`.
- **`ChooseNextThread`:**
    - Local selection is $O(1)$.
    - Work-stealing is $O(S)$ (limited random probes across L3/NUMA/Global domains).
- **Total:** Effectively $O(1)$ for the hot path.

### `enqueue()`
- **Complexity:** $O(1)$ to $O(N)$
- **Details:** Primarily limited by `ChooseCoreAndCPU`. On a system with many cores and high contention, the retry loop is bounded by a small multiple of the CPU count, but the core selection within it follows the $O(1)$/$O(N)$ rules of `choose_core`.

---

## 4. Load Tracking (src/system/kernel/scheduler/scheduler_cpu.cpp & scheduler_thread.cpp)

### `TrackLoad()`
- **Complexity:** $O(1)$
- **Details:** Updates System Virtual Time (SVT) and performance levels using fixed-point arithmetic and atomic updates. No loops over threads or CPUs.

### `_UpdateLoad()` (Core Load)
- **Complexity:** $O(1)$
- **Details:** Uses a combined 64-bit atomic to track load and epoch. No linear scans.

---

## Summary Table

| Function | Complexity (Typical) | Complexity (Worst Case) |
| :--- | :--- | :--- |
| `RunQueue::PeekBest` | $O(1)$ | $O(1)$ |
| `ThreadData::Enqueue` | $O(1)$ | $O(1)$ |
| `choose_core` | $O(1)$ | $O(N)$ (Affinity Scan) |
| `rebalance` | $O(1)$ | $O(N)$ (Affinity Scan) |
| `reschedule` | $O(1)$ | $O(1)$ |
| `TrackLoad` | $O(1)$ | $O(1)$ |
| `Work Stealing` | $O(1)$ | $O(1)$ (Fixed Probes) |
| `UpdatePriorityBoost` | $O(1)$ | $O(1)$ (Fixed budget of bins) |
| `GetTotalRunnableThreads`| $O(N)$ | $O(N)$ (Scans per-CPU counters) |
