# Scheduler Performance Comparison: Haiku (Updated) vs. Haiku (Master) vs. Linux EEVDF

This document provides a detailed performance and architectural comparison between three scheduler implementations:
1.  **Haiku Updated (Audit):** The local Virtual Deadline scheduler.
2.  **Haiku Master (Legacy):** The current O(1) Penalty-based scheduler in the Haiku master branch.
3.  **Linux EEVDF:** The "Earliest Eligible Virtual Deadline First" scheduler (Linux 6.6+), which replaced CFS.

## 1. Algorithmic Complexity & Data Structures

| Metric | Haiku Updated (Audit) | Haiku Master (Legacy) | Linux EEVDF |
| :--- | :--- | :--- | :--- |
| **Algorithm** | Virtual Deadline (O(1) Map) | Penalty-Based Heuristic | EEVDF (Lag-Based) |
| **RunQueue** | **O(1)** (Bitmap + Array) | **O(1)** (Bitmap + Array) | **O(log N)** (Augmented Red-Black Tree) |
| **Enqueue** | Constant Time | Constant Time | Logarithmic Time |
| **Pick Next** | Constant Time (Find Last Set) | Constant Time (Find Last Set) | Logarithmic Time (Tree Traversal) |
| **Scalability**| **Linear** (No global locks) | **Limited** (Global Heaps) | **Linear** (Per-CPU Runqueues) |

*   **Haiku Updated:** Maintains strict O(1) complexity by mapping virtual deadlines to a fixed priority range [0..99]. This allows it to use simple bitmaps instead of trees.
*   **Linux EEVDF:** Uses an Augmented Red-Black Tree to sort threads by virtual deadline. This provides perfect fairness precision but introduces O(log N) overhead, which can become noticeable with very deep queues (thousands of threads).
*   **Haiku Master:** Is also O(1) but relies on "guessing" interactivity via penalties, which breaks down under complex workloads.

## 2. Latency & Fairness Guarantees

| Metric | Haiku Updated (Audit) | Haiku Master (Legacy) | Linux EEVDF |
| :--- | :--- | :--- | :--- |
| **Mechanism** | Deadline Urgency (Bitmap) | Priority Degradation | Eligible Deadline (Tree) |
| **Latency** | **Bounded (Quantum)** | Unbounded (Heuristic) | **Bounded (Lag)** |
| **Fairness** | Statistical (Mapping) | Approximate (Heuristic) | **Precise** (Tree Sorting) |
| **Interactive**| **Immediate Preemption** | Penalty/Boost Logic | Immediate (if eligible) |

*   **Haiku Updated:** Guarantees that interactive tasks (which sleep) accumulate "urgency". Upon waking, they are mathematically guaranteed to be at the front of the queue if their deadline is earlier than the current time. The trade-off is "Statistical Fairness" due to bucketizing deadlines into 99 priority levels.
*   **Linux EEVDF:** Offers the strongest theoretical guarantees. It explicitly tracks "Lag" (how much a task is behind its fair share) to ensure tasks always get their slice. However, the tree management overhead can impact raw wake-up latency on huge systems.
*   **Haiku Master:** Offers no strict guarantees. A batch task could theoretically starve an interactive task if the penalty logic fails to degrade it sufficiently.

## 3. Topology & Work Stealing

| Metric | Haiku Updated (Audit) | Haiku Master (Legacy) | Linux EEVDF |
| :--- | :--- | :--- | :--- |
| **Topology** | **Hierarchical Bitmaps** | Global Lists | Sched Domains (Tree) |
| **Idle Find** | **O(1)** (Bit Scan) | O(N) (List Scan) | O(N) (Domain Walk) |
| **Stealing** | **Active 3-Phase Random** | Passive (Queue Balance) | Active Load Balancing |
| **Locking** | Fine-Grained / Lockless | Global Locks | Per-RQ Locks |

*   **Haiku Updated:** Uses a unique **Hierarchical Bitmap** approach. Finding an idle core is a single CPU instruction (`__builtin_ctz`) on a bitmask, making it significantly faster than Linux's "Sched Domain" walk or Haiku Master's linked list scan.
*   **Linux EEVDF:** Uses complex "Sched Domains" to balance load. While powerful, rebalancing is expensive and often done periodically rather than instantly.
*   **Haiku Updated:** Its "3-Phase Random Stealing" is designed for lower overhead than Linux's comprehensive balancing, prioritizing *finding any work quickly* over *perfectly balancing all queues*.

## 4. Jitter & Throughput

*   **Throughput:**
    *   **Linux EEVDF:** Excellent, but O(log N) tree operations burn slightly more CPU cycles per switch than O(1) bitmaps.
    *   **Haiku Updated:** **Best in Class**. The O(1) enqueue/dequeue combined with cache-affine work stealing minimizes scheduler overhead, leaving more cycles for actual work.
    *   **Haiku Master:** High overhead on large systems due to global lock contention (`gCoreHeapsLock`).

*   **Jitter:**
    *   **Haiku Updated:** **Minimal**. Fine-grained locking and O(1) algorithms ensure predictable execution times.
    *   **Linux EEVDF:** Low, but tree rebalancing can occasionally cause spikes.
    *   **Haiku Master:** High variance due to global lock contention.

## Conclusion

| Feature | Haiku Updated (Audit) | Linux EEVDF | Haiku Master (Legacy) |
| :--- | :--- | :--- | :--- |
| **Primary Goal**| **Low Latency & Simplicity** | **Perfect Fairness** | Desktop Responsiveness |
| **Design** | **Hybrid** (Deadline logic + O(1) structures) | **Pure Deadline** (Tree) | **Heuristic** (O(1)) |
| **Winner** | **Responsiveness / Jitter** | **Fairness** | (Obsolete) |

The **Haiku Updated Scheduler** takes a pragmatic "Hybrid" approach. It adopts the superior fairness logic of EEVDF (Virtual Deadlines) but rejects the expensive Red-Black Trees in favor of O(1) Bitmaps. This results in a scheduler that mimics EEVDF's fairness while retaining the raw speed and low jitter of an O(1) real-time scheduler, making it uniquely suited for high-performance desktop and media workloads.
