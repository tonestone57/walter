# Comparative Analysis: Haiku BMQ-EEVDF vs. Nexus Hybrid Scheduler (NHS)

## 1. Introduction
This report compares the current Haiku scheduler (a decentralized BMQ-EEVDF architecture refined in 2025) with the proposed Nexus Hybrid Scheduler (NHS). The goal is to determine which architecture better serves Haiku's target of uncompromising desktop responsiveness combined with server-grade throughput.

## 2. Architectural Comparison

| Feature | Haiku BMQ-EEVDF (Current) | Nexus Hybrid Scheduler (NHS) |
|---|---|---|
| **Core Engine** | Decentralized BMQ + EEVDF | Bi-Modal (Sprint + Marathon) |
| **Interactivity** | Priority-mapped urgency + Quantum scaling | Explicit "Sprint Layer" (Strict Priority) |
| **Throughput** | EEVDF with load-scaled quantums | "Marathon Layer" (EEVDF) |
| **Locking Model** | Decentralized Per-CPU Spinlocks + Lock-Free Bit-Stealing | Lock-Free Dispatch Stratum (Ring Buffers) |
| **Classification** | Virtual Runtime + Interactivity Score (0-1000) | EWMA Burst History |
| **Work Stealing** | Tiered (L3 -> NUMA -> Global) | Adaptive Work-Stealing Protocol (NUMA-first) |

## 3. Metric-by-Metric Evaluation

### 3.1 Latency & Responsiveness
*   **Haiku BMQ-EEVDF**: Uses a 16-row BMQ matrix where interactive threads (Effective Priority >= 30) are mapped to higher rows. Quantum calculation is "Display-Aware," reducing slices to ~1.2ms when high-priority threads are waiting. EEVDF deadlines ensure that bursty threads stay "eligible" longer.
*   **NHS**: Proposes a "Sprint Layer" which is an O(1) strict-priority array. This bypasses the EEVDF math for interactive tasks entirely, potentially shaving off micro-overhead.
*   **Verdict**: **NHS (Slight Edge)**. While Haiku's BMQ is fast, the explicit bypass of the EEVDF engine for UI threads guarantees the absolute lowest latency.

### 3.2 Throughput
*   **Haiku BMQ-EEVDF**: Seamlessly handles batch tasks via the same EEVDF logic, but scales their quantum up to 3.2ms. Load-scaling automatically reduces quantums under contention to maintain system-wide progress.
*   **NHS**: The "Marathon Layer" is dedicated to throughput. By separating batch tasks from the "Sprint" layer, it can use much larger time-slices without risking UI stutters.
*   **Verdict**: **Tie**. Both use EEVDF for batch fairness. NHS's isolation might prevent batch tasks from ever interfering with UI, but Haiku's integrated approach allows batch tasks to scavenge idle time more fluidly.

### 3.3 Scalability
*   **Haiku BMQ-EEVDF**: Uses decentralized per-CPU run-queues. Lock-Free Bit-Stealing allows a thief CPU to check for work using atomic bitmask operations before ever touching a spinlock. Phase 2 (2025) eliminated global RCU and interaction state contention.
*   **NHS**: Proposes a "Lockless Dispatch Stratum" using per-core lock-free ring buffers.
*   **Verdict**: **NHS (Theoretical Edge)**. Lock-free ring buffers are theoretically superior to per-CPU spinlocks at massive scale (128+ cores). However, Haiku's current decentralized approach with bit-stealing already minimizes contention to near-zero levels on current hardware.

### 3.4 Fairness
*   **Haiku BMQ-EEVDF**: Implements formal EEVDF where virtual runtime and deadlines are weight-proportional.
*   **NHS**: Uses the EEVDF Lag_i fluid-flow model.
*   **Verdict**: **Tie**. They use the same mathematical foundation for fairness.

### 3.5 Cache Locality
*   **Haiku BMQ-EEVDF**: Implements a highly sophisticated tiered stealing policy (L3-first, then NUMA socket, then Global). Interactivity-gated migration prevents UI threads from jumping across NUMA nodes.
*   **NHS**: Topology-aware selection with P/E core steering.
*   **Verdict**: **Haiku (Current Edge)**. Haiku's current implementation is deeply aware of L3/NUMA hierarchies and has explicit guards for UI thread "stickiness." NHS's P/E core steering is a valuable addition for modern hybrid CPUs.

## 4. Conclusion
The **Nexus Hybrid Scheduler (NHS)** represents a logical evolution of the current Haiku scheduler. While Haiku's **BMQ-EEVDF** is already state-of-the-art (decentralized, EEVDF-fair, and cache-aware), NHS offers two key advantages:
1.  **Explicit Policy Separation**: The Sprint/Marathon split simplifies the scheduling hot-path for UI tasks.
2.  **Lock-Free Ring Buffers**: Further future-proofing for "massive multi-core" systems.

## 5. Massive Scaling Analysis (64 to 2048 Cores)

As core counts increase, the overhead of synchronization and the efficiency of the dispatch mechanism become the primary performance drivers.

| Core Count | Haiku BMQ-EEVDF Performance | NHS Performance | Superiority |
|---|---|---|---|
| **64** | **High**: The 64-bit `gIdleMask` is fully utilized. Cache contention on `gTotalRunnableThreads` begins to manifest but is manageable. | **Extreme**: Lock-free dispatch queues (DSQs) eliminate the minor spinlock overhead present in Haiku. | **NHS** |
| **128** | **Architectural Wall**: The `uint64` masks (`gIdleMask`) overflow. Requires conversion to bitset arrays. Global atomic contention becomes a measurable bottleneck. | **Excellent**: DSQs scale linearly. The bi-modal split ensures UI threads aren't delayed by batch processing overhead. | **NHS** |
| **256 - 512** | **Degraded**: `gSchedulerListenersLock` (RW Spinlock) and `gTotalRunnableThreads` cause significant cache-line bouncing across NUMA nodes. | **Robust**: Lock-free Stratum avoids global serialization. Adaptive work-stealing favors local NUMA neighbors first, preserving bandwidth. | **NHS** |
| **1024 - 2048** | **Pathological**: Spinlock-based decentralized queues suffer from "IPI storms" during high-frequency wakeups. Serialized global counters throttle nearly all throughput. | **Sovereign**: Atomic ring-buffer operations (DSQs) handle hundreds of thousands of wakeups/sec without global mutexes. The system remains responsive. | **NHS** |

### Summary of Scaling Advantages
1. **Mask Scalability**: Current Haiku is hard-coded for 64-bit CPU masks. NHS's Stratum is designed for arbitrary core counts via distributed ring buffers.
2. **Synchronization**: Haiku still relies on per-CPU spinlocks for the final "Mechanism" phase. NHS uses **Atomic Pointer Swapping**, which is truly wait-free for the fast path.
3. **Bottleneck Elimination**: NHS eliminates the global `gTotalRunnableThreads` and `gSchedulerListenersLock` bottlenecks by design, distributing accounting into the Layered Engine.

## 6. Final Conclusion & Recommendation
The **Nexus Hybrid Scheduler (NHS)** is substantially superior to the current Haiku scheduler, particularly as systems scale beyond 64 cores. While Haiku's **BMQ-EEVDF** is an excellent modern scheduler for today's desktops, it possesses architectural "knots" (64-bit masks, global atomics, spinlock-based mechanism) that will fail at the massive scales required by 2025-2030 server and workstation hardware.

**Haiku should adopt the NHS architecture.** The decoupling of Policy (Sprint/Marathon) from Mechanism (Lockless Dispatch) is the only viable path to achieving uncompromising desktop responsiveness alongside massive multi-core throughput at the 1024+ core level.
