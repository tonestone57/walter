> **Note:** The performance figures and scaling gains provided in this report are **Analytical Estimates** derived from an architectural audit of the scheduler implementation and known hardware constraints. They represent theoretical potential rather than empirical benchmarks.

# Recommendations for Haiku Scheduler Architecture (v2)

Based on peer review and architectural audit, this revised proposal outlines a production-grade path for integrating **EEVDF (Earliest Eligible Virtual Deadline First)** principles into Haiku's scheduler while maintaining extreme scalability.

## 1. Per-Core Min-Deadline Heaps

**Optimization:** Instead of a single global heap (which creates a massive lock bottleneck), Haiku should implement **Per-Core Min-Deadline Heaps**.

- **Scalability:** Each CPU manages its own local run-queue. Scheduling decisions (`reschedule()`) only require the local CPU's lock, eliminating contention on high-core-count systems.
- **Complexity:** Root selection remains $O(1)$. Heap rebalancing is $O(\log N)$, where $N$ is the number of threads on a *single* core (typically small), ensuring lower overhead than a global structure.
- **Ordering:** Provides perfect local ordering of threads based on their virtual deadlines.

## 2. Formal Service-Lag Tracking & Eligibility

**Fairness:** Replace the "interactivity score" heuristic with mathematical **Lag Tracking**.

- **Lag Definition:** $\text{Lag} = \text{ServiceReceived} - \text{FairShare}$.
- **Eligibility:** A thread only enters the heap when it is **eligible** ($\text{VirtualRuntime} \le \text{SystemVirtualTime}$).
- **Benefit:** Prevents bursty threads from dominating the CPU. They are only "re-paid" what they are mathematically owed, not rewarded blindly for sleeping.

## 3. Decoupling Latency (Requests) from Throughput (Weights)

**deterministic QoS:** Implement the EEVDF deadline formula:
$\text{Deadline} = \text{VirtualTime} + (\text{RequestSize} / \text{Weight})$

- **Impact:** An audio thread can request a small `RequestSize` (e.g., 500µs latency) with a small `Weight` (5% CPU share). It will run with high urgency but will be pre-empted or made ineligible if it tries to consume more than its 5% share.

## 4. Dynamic Preemption Granularity

**Throughput Protection:** Instead of adjusting "buckets" (which no longer exist in a continuous heap), we adjust the **Preemption Threshold**.

- **Mechanism:** A newly eligible thread with an earlier deadline will only preempt the current thread if the difference in their deadlines exceeds a dynamic threshold ($\Delta_{deadline} > \epsilon$).
- **Adaptive Control:** On an idle system, $\epsilon$ is near zero (instant response). On a heavily loaded server, $\epsilon$ increases to reduce context-switching frequency and preserve Instruction Cache (I-Cache) performance.

---

## 5. Hierarchical Work-Stealing and Migration

To balance load across per-core heaps without a global lock, Haiku will use a **Thief-Initiated Hierarchical Strategy**.

### 5.1 The 3-Phase Steal (Lag-Based)
When a CPU becomes idle, it enters the work-stealing path:
1.  **Phase 1 (Sibling):** Try to steal from a sibling core sharing the same L2/L3 cache.
2.  **Phase 2 (Local NUMA):** Randomly sample cores within the same physical NUMA node.
3.  **Phase 3 (Global Hail Mary):** Sample remote nodes using "Power of Two Choices" random sampling.

### 5.2 Steal Criteria: "The Laggiest Wins"
Unlike traditional schedulers that steal the "highest priority" thread, the EEVDF-aware thief prefers threads with the **highest positive Lag** (most under-served).

- **Migration Decision:** A thread is migrated if its lag is significantly positive AND the target core has lower total weight pressure.
- **Cache Locality:** Migration across NUMA boundaries is discouraged by increasing the "Lag Threshold" required for a successful steal, preserving cache warmth for threads that are home.

---

## Summary Verdict

| Feature | Change | Result |
| :--- | :--- | :--- |
| **RunQueue** | Bucketed $\to$ Per-Core Heap | $O(1)$ selection, No Lock Contention |
| **Fairness** | Heuristic $\to$ Lag Tracking | Mathematical proof of fairness |
| **Latency** | Coupled $\to$ Decoupled | Low latency without over-allocation |
| **Preemption** | Static $\to$ Dynamic Threshold | Throughput-aware responsiveness |

This architecture combines the formal fairness of EEVDF with the high-performance, decentralized design required for modern multi-core processors.
