# Scheduler Comparison: Current Method vs. Hybrid (Weighted Vruntime + Latency Bonus)

This document analyzes the proposed "Hybrid" scheduler design against the current Haiku scheduler implementation.

## The Hybrid Design Concept
**Weighted Vruntime with Latency Bonus**
*   **Structure:** A single Red-Black Tree for all threads (Priority 1–99).
*   **Sorting Key:** `vruntime` (Virtual Runtime). Smallest runs next.
*   **Mechanism:**
    *   **Weighted Growth:** `vruntime` grows slower for high-priority tasks.
    *   **Latency Bonus:** On wake-up, interactive tasks receive a "bonus" (deduction from `vruntime` or placement optimization) to effectively "jump the queue" and run immediately, replicating the snappy feel of strict priority systems.

## Comparison Table

| Metric | Current Method (Haiku O(1)) | Hybrid Design (Weighted Vruntime + Latency Bonus) |
| :--- | :--- | :--- |
| **Starvation** | **Possible.** Strict priority means low-priority threads never run if high-priority is busy (mitigated by boosting). | **Eliminated.** Weighted Fair Queuing guarantees all threads get CPU time proportional to their weight. |
| **Latency (Scheduling)** | **O(1) Constant.** Bitmap scan + Priority Queue is extremely fast and deterministic. | **O(log N).** Red-Black Tree operations have logarithmic overhead. Slightly slower dispatch. |
| **Throughput** | **High.** Minimal scheduling overhead. Strict priority can cause "convoy effects" where low-prio tasks holding resources stall the system. | **Balanced.** Slightly higher scheduling overhead, but better overall system utilization by avoiding convoys and starvation. |
| **Responsiveness** | **Excellent (Strict).** High-priority interactive apps *always* preempt lower tasks immediately. | **Excellent (Bonus).** "Latency Bonus" ensures interactive tasks jump to the front of the tree, mimicking strict priority responsiveness. |
| **Deadlock Risk** | **High (Priority Inversion).** Strict priority makes it easy for a high-prio task to starve while waiting for a low-prio lock holder. | **Low.** Even low-priority lock holders get *some* CPU time, allowing them to progress and release locks. |

## Detailed Analysis

### 1. Starvation
*   **Current:** Uses 100 separate queues. If a thread at Priority 20 is running, a thread at Priority 10 *cannot* run. This is "Strict Priority." Haiku uses an "aging" mechanism (Priority Boosting) to eventually boost starving threads, but it is a reactive patch.
*   **Hybrid:** Uses a single tree. A low-priority thread has a "heavy" vruntime weight, meaning it moves to the right of the tree faster, but it is *in the tree*. It will eventually be the leftmost node. Starvation is mathematically impossible in a fair queuing model.

### 2. Latency (Scheduling Overhead)
*   **Current:** Finding the next task is finding the first set bit in a bitmap (`fls`) and picking the head of a linked list. This is effectively instantaneous and constant time regardless of thread count.
*   **Hybrid:** Requires tree insertion and deletion, which are `O(log N)`. For 1000 threads, this is ~10 operations. While slower than O(1), on modern CPUs this difference is often negligible compared to the benefits of better decision making.

### 3. Throughput
*   **Current:** Extremely low overhead maximizes theoretical CPU cycles available for work. However, strict priority can hurt *system* throughput if a low-priority I/O thread is starved, preventing it from fetching data for a high-priority calculation.
*   **Hybrid:** The overhead is higher, but the "Fairness" ensures that helper threads and background workers make steady progress, potentially improving the throughput of complex, multi-stage pipelines.

### 4. Responsiveness
*   **Current:** The defining characteristic of BeOS/Haiku. The mouse cursor (high priority) never stutters because it *strictly* overrides compilation (low priority).
*   **Hybrid:** Standard CFS can feel "sluggish" because it waits for the interactive task to "earn" its place. The **Latency Bonus** is the critical "Turbocharger": it artificially places waking interactive tasks at the far left of the tree, forcing an immediate preemption. This aims to replicate the "snappy" feel within a fair framework.

### 5. Deadlock Risk (Priority Inversion)
*   **Current:** A classic problem in strict priority systems. If a Low Priority task holds a mutex required by a High Priority task, but a Medium Priority task is running, the Low task never runs to release the lock. The High task hangs forever. Haiku employs Priority Inheritance to fix this, but it adds complexity to the locking primitives.
*   **Hybrid:** Naturally resilient. The Medium task runs, but the Low task also gets a small slice of time. It eventually releases the lock. Priority Inheritance is still good, but the system is less brittle without it.
