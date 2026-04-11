# Scheduler Comparison: Haiku VD vs. Linux EEVDF vs. Redox DWRR

This document compares the architectural performance of the Haiku Virtual Deadline (VD) scheduler, Linux EEVDF (Earliest Eligible Virtual Deadline First), and Redox DWRR (Deficit Weighted Round Robin).

## 1. Context Switching & Selection Overhead

| Scheduler | Complexity | Mechanism |
| :--- | :--- | :--- |
| **Haiku VD** | **O(1)** | Uses a bitmap-based `RunQueue` with `fls` (Find Last Set) to identify the highest priority level instantly. Within a level, it performs a bounded scan (depth 32) for the best candidate. |
| **Linux EEVDF** | **O(log N)** | Uses an augmented Red-Black Tree. Selection (`pick_next_task`) requires traversing the tree to find the earliest eligible deadline. |
| **Redox DWRR** | **O(1)** | Uses distributed weighted round-robin queues. Selection is a simple pointer update to the next task in the queue with a positive deficit. |

**Impact:** Haiku and Redox have lower per-switch overhead than Linux, which becomes significant as the number of runnable tasks ($N$) increases.

## 2. Latency (Dispatch & Wake-up)

*   **Haiku VD:** Employs hierarchical idle core bitmasks (Package -> Node -> Root). Finding an idle core is a constant-time **O(1)** operation regardless of system size. Preemption is immediate for "urgent" (interactive) tasks.
*   **Linux EEVDF:** Uses `sched_domains` for wake-up placement. While highly optimized, it often requires traversing topology levels (SMT, Core, Die, NUMA), leading to **O(Topology Depth)** latency.
*   **Redox DWRR:** Focuses on proportional fairness. While latency is bounded by the round-robin cycle, it lacks the explicit "urgency" or "deadline" priority of the other two, potentially leading to higher jitter for interactive tasks under heavy load.

## 3. Responsiveness (User Interaction)

*   **Haiku VD:** **High**. Interactive tasks (which sleep frequently) accumulate "urgency" during sleep. Upon waking, they map to high dynamic priorities and preempt background tasks instantly.
*   **Linux EEVDF:** **High**. EEVDF calculates "lag" (service received vs. deserved). Sleeper tasks wake up with positive lag (under-serviced), making them immediately "eligible" and giving them early deadlines.
*   **Redox DWRR:** **Moderate**. Responsiveness is tied to task weight. Without specific interactive boosting, a UI thread might be treated similarly to a high-weight batch process.

## 4. Throughput (System Efficiency)

*   **Haiku VD:** Optimized for massive scale (4096+ cores). Uses **"Power of Two Choices"** random sampling for load balancing to avoid the cache line bouncing and lock contention of global scans.
*   **Linux EEVDF:** Excellent on standard hardware (up to 128-256 cores). At extreme scales, the overhead of maintaining global fairness invariants and domain-based balancing can consume significant CPU cycles.
*   **Redox DWRR:** **Very High**. The algorithmic simplicity of DWRR results in the lowest "scheduler tax" per CPU cycle, allowing more time for user-space applications.

## 5. Scalability & Topology Awareness

*   **Haiku VD:** Designed specifically for NUMA-heavy systems. 3-phase work stealing (Local -> NUMA-Random -> Global-Random) ensures high utilization without global bottlenecks.
*   **Linux EEVDF:** The gold standard for complex, non-uniform topologies (e.g., heterogeneous cores, complex L3 sharing). However, its deterministic domain balancing starts to struggle with "thundering herd" issues on 1024+ core systems.
*   **Redox DWRR:** Scales linearly via distributed queues but lacks the deep hierarchical awareness found in Haiku and Linux, which can lead to sub-optimal placement on complex multi-socket NUMA systems.

---

## 6. Summary Performance Matrix

| Metric | Haiku VD | Linux EEVDF | Redox DWRR |
| :--- | :--- | :--- | :--- |
| **Selection Speed** | Extremely Fast | Fast | Extremely Fast |
| **Idle Core Discovery** | O(1) (Tree) | O(D) (Scan) | O(1) (Local) |
| **Scaling (1k+ Cores)** | Excellent | Moderate | Good |
| **Interactive Feel** | Snappy | Precise | Fair |
| **Throughput** | High | High | Very High |
| **Fairness Model** | Statistical | Mathematical (Lag) | Proportional (Weight) |

**Conclusion:**
*   **Haiku VD** is the best choice for **Large-Scale Multi-Socket Servers** and high-responsiveness desktop use.
*   **Linux EEVDF** provides the most **Mathematically Precise Fairness** and excels on desktop/mobile with heterogeneous cores.
*   **Redox DWRR** offers the **Lowest Overhead**, making it ideal for microkernels and systems where raw throughput is the primary goal.
