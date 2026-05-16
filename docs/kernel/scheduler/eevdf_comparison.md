> **Note:** The performance figures and scaling gains provided in this report are **Analytical Estimates** derived from an architectural audit of the scheduler implementation and known hardware constraints. They represent theoretical potential rather than empirical benchmarks.

# Comparison: Current Virtual Deadline vs. EEVDF

Haiku's current scheduler (2025 Audit version) uses virtual deadlines, but its implementation differs fundamentally from the **EEVDF (Earliest Eligible Virtual Deadline First)** algorithm. This report outlines the technical improvements EEVDF would provide and explains the architectural decisions behind priority buckets.

## 1. Current Implementation: "Deadline-to-Priority" Mapping

The current scheduler is a **Priority-Based Preemptive Scheduler** that uses deadlines as a heuristic for urgency.

- **Mechanism (`scheduler_thread.cpp`):** `_UpdateDeadline()` calculates a future timestamp based on static priority and interactivity score. `_ComputeEffectivePriority()` then maps the remaining time (`deadline - now`) into a discrete dynamic priority level (0-99).
- **Ordering (`RunQueue.h`):** The `RunQueue` is a multi-level priority queue. Threads in higher priority buckets always preempt or run before lower ones.
- **Role of Deadlines:** Deadlines are used to determine which bucket a thread belongs to. Within a bucket, `ThreadDataVRuntimeCompare` ensures the thread with the lowest virtual runtime is preferred, but the strict bucket separation remains.

## 2. EEVDF: Proportional Share with Decoupled Latency

EEVDF is a **Fair-Queueing Scheduler** that replaces fixed priority levels with mathematical share and request bounds.

### 2.1 Decoupling Throughput from Latency
- **Current:** If an audio thread needs low latency (300µs), it must be assigned a high priority. This high priority also grants it a large slice of the CPU (high throughput), even if it only does a tiny amount of work.
- **EEVDF:** Latency and throughput are independent parameters. A thread can have a **low weight** (2% of the CPU) but a **short request** (500µs latency). This allows interactive background tasks to be responsive without over-allocating CPU throughput.

### 2.2 Strict Eligibility & Lag Tracking
- **Current:** The interactivity score (`fInteractivityScore`) rewards bursty threads via a heuristic.
- **EEVDF:** It explicitly tracks **Lag** (the difference between a thread's fair share and its actual service). A thread only becomes **eligible** when its virtual runtime is $\le$ the system's virtual time, preventing "bursty" threads from unfairly dominating the CPU after waking up.

---

## 3. Improving Current Virtual Deadlines

Based on the local codebase (`src/system/kernel/scheduler/`), the current virtual deadline implementation could be improved further:

1.  **Deadline-Sorted Run Queues (Binary Heaps):**
    Currently, `RunQueue::PeekBest` uses a lookahead heuristic (scanning up to 3 non-empty priority levels and 32 threads each). Replacing the bucketed `RunQueue` with a **Binary Heap of Deadlines** would allow the scheduler to find the absolute earliest deadline in O(1) and insert/remove in O(\log N), eliminating the "aliasing" of deadlines into discrete priority buckets.
2.  **Fine-Grained Virtual Time Tracking:**
    Integrating the `fVirtualRuntime` tracking more deeply into the main scheduling loop (rather than just for intra-bucket sorting) would bring the system closer to Fair Queueing accuracy.
3.  **Formal Lag Calculation:**
    Replacing the heuristic `fInteractivityScore` with a formal lag-based eligibility check would provide more consistent behavior for media workloads.

---

## 4. Architectural Choice: 32 vs. 99+ Priority Buckets

The question of why many schedulers use 32 levels (0-31) instead of 100+ (0-99) is a trade-off between **Hardware Efficiency** and **Granularity**.

### 4.1 The Efficiency of 32 (Standard Desktop OS)
- **O(1) Bitmask Search:** On many architectures, a single 32-bit CPU register can represent 32 priority levels. Finding the highest priority is a single instruction (`ffs` or `clz`).
- **Cache Locality:** An array of 32 bucket pointers (heads/tails) fits perfectly in a few cache lines.

### 4.2 The 0-99 Expansion in the Local Audit
The local 2025 Audit implementation explicitly expands this to a **0-99 range** for dynamic mapping.

- **Implementation (`RunQueue.h`):** The `RunQueue` is templated to handle arbitrary `MaxPriority`. It uses a multi-word bitmap:
  `static const int kBitmapSize = (MaxPriority + 32) / 32;`
  For the local `THREAD_MAX_SET_PRIORITY` (120), the bitmap is 4 words long.
- **Why 99?** As seen in `scheduler_thread.cpp`, `kMaxDynamicPriority` is set to 99 (`B_FIRST_REAL_TIME_PRIORITY - 1`).
- **The Benefit:** By using 100 dynamic levels instead of 32, we reduce "bucket collisions." Two threads with slightly different deadlines are less likely to be forced into the same bucket, allowing the scheduler to differentiate between them more accurately.
- **The Cost:** The search loop in `_FindNextPriority` must now check up to 4 words. On modern CPUs, this extra work is negligible compared to the benefit of reduced latency jitter for interactive threads.

## Conclusion
The shift from 0-31 to 0-99 buckets in the local implementation reflects the transition from hardware-constrained kernels to those optimized for modern, high-precision interactive workloads. While EEVDF represents the theoretical "next step," the current 100-level deadline mapping provides a highly responsive environment for Haiku today.

---

## 5. Bridging the Gap: Performance and Fairness Convergence

Integrating Deadline-Sorted Heaps and Formal Lag Tracking into the current virtual deadline model brings the Haiku scheduler significantly closer to EEVDF.

### 5.1 Fairness Convergence (~90%)
By moving from heuristic buckets to a continuous deadline sorting mechanism and implementing formal lag tracking, the scheduler achieves **near-parity with EEVDF fairness.**

- **The "Urgency" vs. "Eligibility" Shift:** Currently, deadlines only indicate urgency (when a thread *should* run). With lag tracking, the scheduler can implement an eligibility barrier (when a thread *is allowed* to run). This prevents bursty threads from receiving more than their fair share, matching EEVDF's core principle.
- **Latency Bounds:** The use of O(1) deadline selection from a heap provides the same deterministic latency bounds as EEVDF, ensuring audio threads always run at the absolute earliest possible moment.

### 5.2 Performance Trade-offs
The transition involves a fundamental trade-off between **Hot Path Complexity** and **Dispatch Efficiency.**

| Metric | Current (100 Buckets) | Improved (Deadline Heaps) | Formal EEVDF |
| :--- | :--- | :--- | :--- |
| **Selection Complexity** | O(1) (with lookahead) | **O(1)** (from heap root) | O(log N) |
| **Insertion Complexity** | O(1) | **O(log N)** | O(log N) |
| **Memory Locality** | Excellent (Array of words) | Moderate (Tree pointers) | Moderate |
| **Fairness Precision** | Bucket-level (Aliasing) | **Perfect (Continuous)** | Perfect |

- **Hot Path Impact:** Replacing the multi-word bitmap search (O(1)) with a heap insertion (O(log N)) adds a small number of CPU cycles to the `enqueue()` path. However, for a typical desktop thread count (N=256), `log2(256) = 8`, making the cost negligible on modern 3GHz+ processors.
- **Dispatch Benefit:** The current `PeekBest` lookahead is a heuristic that can sometimes pick a sub-optimal thread or require multiple lock attempts. A heap-based selection is **authoritative and instantaneous**, potentially saving more time in the dispatcher than was lost in the enqueuer.

### 5.3 Summary Verdict
With these improvements, the Haiku scheduler would be **functionally equivalent to EEVDF** for the vast majority of workloads. The only remaining "gap" is the formal proof of absolute fairness that comes with a pure EEVDF implementation, but for a real-world desktop OS, the performance/fairness balance of an **Improved Virtual Deadline** model is the superior engineering choice.

---

## 6. Optimizing Bucket-Level Fairness

While expanding to 100 priority buckets reduces collisions, the fairness *within* a single bucket can still be improved. Currently, Haiku uses unsorted linked lists per bucket, with a 32-thread scan limit (`kSearchDepth`) in `PeekBest`.

### 6.1 The "Aliasing" Problem
When multiple threads have similar deadlines and fall into the same bucket, their execution order is determined by their position in the linked list (usually the order they were woken up). If a bucket contains more than 32 threads, the current scheduler may ignore an earlier deadline thread simply because it is too far back in the list.

### 6.2 Proposed Intra-Bucket Improvements

1.  **Sorted Bucket Insertion:**
    - **Mechanism:** Instead of `PushBack`, implement a sorted insertion based on `fVirtualRuntime`.
    - **Complexity:** Shifts cost from dispatching O(32) scan) to enqueuing O(N_{bucket})).
    - **Benefit:** Makes `PeekBest` a true O(1) operation (always pick the head), ensuring that the absolute best thread in the bucket is always chosen without arbitrary scan limits.
2.  **Red-Black Tree Buckets:**
    - **Mechanism:** Replace the per-priority linked lists with Red-Black trees (as seen in Linux CFS).
    - **Complexity:** O(\log N) for both enqueuing and selection.
    - **Benefit:** Provides perfect fairness even for buckets with thousands of threads. However, for a desktop OS where buckets rarely exceed 10-20 threads, the pointer-chasing overhead of a tree may exceed the cost of a simple linked-list insertion.
3.  **Adaptive Lookahead:**
    - **Mechanism:** Scale the `kSearchDepth` dynamically based on the total thread count in the bucket or the system's current load.
    - **Benefit:** Allows the scheduler to spend more time finding the "perfect" thread when the system is not heavily congested, while maintaining strict O(1) bounds during extreme overload.

### 6.3 Summary Comparison of Fairness Models

| Strategy | Fairness Precision | Enqueue Cost | Dispatch Cost | Scalability |
| :--- | :--- | :--- | :--- | :--- |
| **Current (Unsorted)** | Low (Scans only top 32) | **O(1)** | O(32) | Poor in deep buckets |
| **Sorted Insertion** | **High (Perfect in-bucket)** | O(N) | **O(1)** | Good for small/med buckets |
| **RB-Tree Buckets** | **High (Perfect in-bucket)** | O(log N) | O(log N) | Best for massive buckets |
| **Deadline Heaps** | **Absolute (Global)** | O(log N) | **O(1)** | Best overall fairness |

**Verdict:** For Haiku's interactive focus, **Sorted Bucket Insertion** is the logical next step. It provides perfect bucket-level fairness with minimal architectural change, ensuring that threads with the earliest deadlines always run first within their assigned priority band.
