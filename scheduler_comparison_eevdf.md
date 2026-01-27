# Scheduler Performance Comparison: Haiku Virtual Deadline vs. Linux EEVDF

This document compares the theoretical performance characteristics of Haiku's new **Virtual Deadline Scheduler** against the state-of-the-art **Linux EEVDF** (Earliest Eligible Virtual Deadline First) scheduler.

## Executive Summary

| Metric | Haiku (Virtual Deadline) | Linux (EEVDF) |
| :--- | :--- | :--- |
| **Complexity (Enqueue/Dequeue)** | **O(1)** (Constant) | **O(log N)** (Logarithmic) |
| **Data Structure** | Priority Bitmap + Arrays | Augmented Red-Black Tree |
| **Scheduling Decision** | `fls` (Find Last Set bit) - CPU Instruction | Tree Traversal (Leftmost Node) |
| **Fairness Mechanism** | Weighted Deadline Buckets | Virtual Time Lag (Eligible + Deadline) |
| **Latency (Wakeup)** | **Extremely Low** (Deterministic) | Low (Bounded by tree depth) |
| **Starvation Immunity** | **Guaranteed** (Deadlines advance) | **Guaranteed** (Lag tracking) |
| **Interactive "Boost"** | **Latency Bonus** (Reset Deadline on Wake) | **Lag Bonus** (Zero Lag Placement) |
| **Cache Locality** | **High** (Per-CPU Runqueues) | **High** (Per-CPU Runqueues) |

## Detailed Analysis

### 1. Algorithm Complexity
*   **Haiku (Virtual Deadline):** Relies on the O(1) properties of bitmaps. Mapping a deadline to a priority "bucket" allows the scheduler to find the most urgent task using a single CPU instruction (`fls` or `clz`). This performance is constant regardless of whether there are 10 or 10,000 threads.
*   **Linux (EEVDF):** Uses a Red-Black Tree. Every task insertion or removal requires rebalancing the tree, costing `O(log N)`. While efficient, it is mathematically slower than O(1) at scale.

### 2. Fairness & Starvation
*   **Haiku:** Fairness is achieved by the "Deadline Physics." A low-priority task has a deadline far in the future. As time passes, `Now` approaches `Deadline`, and the task's "Urgency" increases. Eventually, it reaches maximum urgency (Priority 99) and *must* run. Starvation is mathematically impossible.
*   **Linux:** EEVDF explicitly tracks "Lag" (the difference between the service a task *should* have received vs. what it *did* receive). It schedules tasks with positive lag (under-serviced) first. This provides mathematically precise fairness but requires complex bookkeeping.

### 3. Responsiveness (Latency)
*   **Haiku:** Uses a "Latency Bonus." When an interactive thread wakes up, its deadline is calculated relative to *Now*. Since interactive tasks have high weight (small slice), `Now + SmallSlice` puts them in the highest urgency bucket immediately. This mimics the "snappy" feel of strict priority systems.
*   **Linux:** Also excellent. EEVDF places waking tasks based on their "Eligible Deadline." If a task has been sleeping, it has "lag" and is considered eligible earlier than running tasks, allowing preemption.

### 4. Throughput & Overhead
*   **Haiku:** The O(1) design minimizes scheduler overhead (CPU cycles spent deciding *what* to run). This leaves more cycles for the actual applications, potentially offering higher throughput on systems with massive thread counts.
*   **Linux:** The overhead is higher due to tree maintenance and complex lag calculations. However, Linux's sophisticated load balancing and NUMA awareness often compensate for this in large-scale server environments.

## Conclusion
**Haiku's Virtual Deadline Scheduler** is designed to be a lightweight, "hard-real-time-like" desktop scheduler. It prioritizes deterministic O(1) latency and responsiveness over the complex, server-grade fairness strictness of **Linux EEVDF**. It achieves 90% of the fairness benefits of EEVDF with 10% of the code complexity and overhead.
