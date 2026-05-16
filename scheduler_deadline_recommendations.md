# Recommendations for Optimizing Virtual Deadlines

To achieve the highest performance and mathematical fairness in Haiku's scheduler, the following architectural changes to the virtual deadline mechanism are recommended.

## 1. Move to Global Min-Deadline Heaps

**Problem:** Currently, deadlines are mapped to 100 discrete priority buckets. Within these buckets, the scheduler uses a "best-of-32" lookahead heuristic. This results in "aliasing," where threads with different deadlines are treated identically, and sub-optimal threads may be chosen if the list is long.

**Recommendation:** Replace the bucketed \`RunQueue\` with a **Binary Min-Heap of Deadlines**.
- **Performance:** Thread selection becomes a true $O(1)$ operation (peek root). Insertion and deletion move to $O(\log N)$.
- **Fairness:** The scheduler will always execute the thread with the absolute earliest deadline across the entire system (or NUMA node), providing perfect ordering without the overhead of scanning buckets.

## 2. Implement Formal Service-Lag Tracking

**Problem:** The current "interactivity score" is a heuristic that attempts to reward bursty threads. It lacks an objective measure of how much CPU time a thread was mathematically "owed."

**Recommendation:** Adopt **Lag-Based Eligibility** (from the EEVDF model).
- **Mechanism:** Track the difference between a thread's *fair share* (based on its weight) and its *actual service*.
- **Benefit:** A thread only becomes "eligible" for scheduling when its virtual runtime is less than or equal to the system virtual time. This prevents bursty threads from unfairly dominating the CPU after waking up, as they are only allowed to "catch up" to their fair share, not exceed it.

## 3. Decouple Latency (Requests) from Throughput (Weights)

**Problem:** Latency and throughput are currently coupled via the priority level. A thread that needs low latency is forced to have a high priority, which also gives it a large share of the CPU.

**Recommendation:** Allow threads to specify a **Latency Request** independent of their **Weight Share**.
- **Mechanism:** The virtual deadline should be calculated as \`Deadline = VirtualTime + (RequestSize / Weight)\`.
- **Benefit:** An audio thread can request a tiny \`RequestSize\` (300µs latency) but have a small \`Weight\` (2% total CPU). It will be scheduled with high urgency whenever it has work but will be prevented from consuming more than its 2% share if it becomes runaway.

## 4. Per-Core Deadline Resolution

**Problem:** \`gDeadlineBucketSize\` is currently a global constant.

**Recommendation:** Implement **Load-Adaptive Resolution**.
- On idle systems, use a high-resolution bucket (e.g., 500µs) to minimize jitter.
- On heavily loaded systems, automatically scale the resolution down (e.g., 5ms) to reduce the frequency of rebalancing and context switching, preserving throughput for heavy compilation or rendering tasks.

---

## Summary of Impact

| Change | Performance Impact | Fairness Impact | Complexity |
| :--- | :--- | :--- | :--- |
| **Min-Deadline Heaps** | Higher (O(1) Selection) | Perfect Ordering | Medium |
| **Lag Tracking** | Low Overhead | Prevents Starvation | High |
| **Decoupled Requests** | Minimal | Guarantees QoS | High |
| **Adaptive Res.** | Power/Throughput Gains | Low | Low |

**Final Verdict:** Transitioning the virtual deadline model from a "Priority Hint" to a **Formal Fair-Queueing Prerequisite** is the most effective path for Haiku. This provides the deterministic performance required for a modern media-centric OS while maintaining the responsiveness Haiku is known for.
