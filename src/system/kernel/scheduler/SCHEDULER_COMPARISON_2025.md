# Scheduler Comparison Report (2025)

This report compares the current repository scheduler (Modernized BMQ-EEVDF) with the Linux EEVDF scheduler and the legacy Haiku OS affine scheduler.

## 1. Architectural Comparison

| Feature | Repo Scheduler (BMQ-EEVDF) | Linux EEVDF | Haiku Legacy Affine |
| :--- | :--- | :--- | :--- |
| **Core Algorithm** | Modernized EEVDF | EEVDF (replaced CFS) | Soft Affinity Priority |
| **Data Structure** | 512-Lane Flat Bitmask | Red-Black Tree ($O(\log N)$) | Doubly Linked Lists |
| **Selection Speed** | $O(1)$ (Branchless) | $O(\log N)$ | $O(1)$ per band |
| **Fairness Model** | Weighted + Capacity Scaled | Pure Weighted Lag | Heuristic |
| **Cache Footprint** | 64 Bytes (1 cache line) | Variable (Multiple lines) | ~128+ Bytes |
| **Interactivity** | Explicit Score (0-1000) | Implicit (Lag-based) | Priority Boosting |

## 2. Performance Summary & Scalability

The following table summarizes the projected "fastest" scheduler (in terms of context-switch latency and throughput) at various core counts.

| Cores | Fastest Scheduler | Justification |
| :--- | :--- | :--- |
| **1** | **Legacy Affine** | Minimal complexity, zero overhead. |
| **4** | **Repo BMQ-EEVDF** | $O(1)$ selection and UI-focused interactivity give it an edge in responsiveness. |
| **8** | **Linux EEVDF** | Linux's maturity in cache-balancing starts to show here. |
| **12 - 32** | **Repo BMQ-EEVDF** | Single-cache-line metadata ensures selection remains "hot" in L1. |
| **64 - 128** | **Repo BMQ-EEVDF** | Decentralized runnable accounting and per-CPU DPCs eliminate global lock contention. |
| **256 - 512** | **Repo BMQ-EEVDF** | Hierarchical work-stealing and NUMA-aware clustering maintain performance where RB-trees become deeper. |
| **1024** | **Linux EEVDF*** | Linux has significantly more enterprise hardening for this scale. Haiku requires **Cellular Sharding** to compete here. |

### Analysis:
- **Low Core Counts (1-8)**: The performance difference is negligible. The Legacy Affine is the simplest, but Modernized BMQ-EEVDF's 512-lane matrix is optimized enough to match it while providing much better fairness.
- **Medium Core Counts (16-128)**: Modernized BMQ-EEVDF excels because selection metadata fits in a single cache line (64 bytes). Linux's RB-tree requires traversing multiple nodes, often triggering multiple L1/L2 cache misses.
- **High Core Counts (256-1024)**: Haiku's decentralized design (RCU listeners, per-CPU DPC) scales linearly. However, at 1024 cores, $O(N)$ bitmask scans for idle cores become a bottleneck.

## 3. How to Further Improve the Current Scheduler

To maintain Haiku's performance lead and address the 1024-core bottleneck, the following improvements are recommended:

### A. Cellular Sharding (Future Roadmap)
Break the system into independent **32-core cells**.
- **Benefit**: Eliminates $O(N)$ overhead of scanning system-wide `CPUSet` bitmasks. Context switch latency remains constant regardless of total system core count.
- **Impact**: Crucial for 1024+ core scalability.

### B. Hardware-Guided EAS (Energy-Aware Scheduling)
Integrate hardware performance counters and efficiency hints directly into the rebalancing logic.
- **Benefit**: More accurate thread placement on hybrid (P/E core) architectures without relying on heuristic "thread coloring."
- **Impact**: Improved battery life and thermal management on modern laptops.

### C. Hierarchical CPUSet Iteration
Replace linear bitmask scans with a hierarchical summary tree (e.g., bits representing "Any Idle in Node").
- **Benefit**: Reduces the cost of finding an idle core from $O(N)$ to $O(\log N)$ or $O(1)$ relative to the number of nodes.
- **Impact**: Immediate performance boost for 128-512 core systems.

### D. Adaptive Quantum Dampening
Dynamically adjust the interactivity-based quantum scaling based on the current context-switch frequency.
- **Benefit**: Reduces "thrashing" during rapid UI events (e.g., window resizing) by preventing too-frequent EEVDF matrix updates.
- **Impact**: Smoother UI performance under heavy load.
