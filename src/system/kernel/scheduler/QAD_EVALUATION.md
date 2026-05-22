# Haiku Scheduler Evaluation: BMQ-EEVDF vs. Quantized Asymmetric Deadline (QAD)

## 1. Overview
This report evaluates the current Haiku scheduler (BMQ-EEVDF) against the proposed Quantized Asymmetric Deadline (QAD) scheduler. The goal is to determine which architecture is better suited for Haiku OS, particularly regarding performance, scalability, and responsiveness.

## 2. Architectural Comparison

### A. Selection Complexity and Data Structures
| Feature | BMQ-EEVDF (Current) | QAD (Proposed) |
| :--- | :--- | :--- |
| **Selection Speed** | $O(1)$ (16x32 bitmask matrix) | $O(1)$ (128-lane bitmask) |
| **Resolution** | 512 discrete bins per CPU | 128 discrete lanes per CPU |
| **Fairness** | Formal EEVDF (Lag-based) | Quantized Lag Fairness |
| **Complexity** | Hybrid: Bitmask + Doubly Linked Lists | Dual-Array Bitmask (Active/Expired) |

**Analysis**: Both schedulers achieve $O(1)$ selection speed. BMQ-EEVDF's 512-bin matrix provides significantly higher resolution than QAD's 128 lanes, allowing for more precise deadline tracking without increasing algorithmic complexity.

### B. Heterogeneous Core Support (P vs. E Cores)
- **BMQ-EEVDF**: Uses `fCapacity` and `fPerformanceScale` metrics. Virtual runtime is updated as `delta = (active * 1000000LL) / weight`, where weight is adjusted by core capacity. Quantum lengths are also scaled by core load and capacity.
- **QAD**: Scales virtual runtime accumulation by `(actual_duration * 1024) / core_capacity`.

**Analysis**: Both architectures natively support asymmetric topologies. BMQ-EEVDF's current implementation is more comprehensive, incorporating capacity into load balancing, priority boosting, and quantum scaling, whereas the QAD proposal focuses primarily on runtime scaling.

### C. Interactivity and Responsiveness
- **BMQ-EEVDF**: Employs an **Interactivity Score (0-1000)** based on thread behavior (e.g., yields, preemption). This score dynamically adjusts urgency (effective priority), quantum lengths, and grants a "Quick Start Credit" for responsive wakeups.
- **QAD**: Proposed **Interactive Latency Credits** which roll back virtual runtime upon wakeup, pushing threads into higher-priority lanes.

**Analysis**: QAD's credit system is a simplified version of what Haiku already implements. Haiku's interactivity score provides a smoother gradient of responsiveness and integrates directly with the formal EEVDF deadline math.

### D. Scalability and Work-Stealing
- **BMQ-EEVDF**: Hierarchical "Laggiest Wins" stealing across L3, NUMA, and Global domains. Thresholds are dynamically calibrated at boot based on interconnect latency.
- **QAD**: Proposes **Cellular Autonomy** (sharding into 32-core cells) and hierarchical bitmask tokenization for extreme scales (up to 4096 cores).

**Analysis**: QAD's cellular sharding is a theoretically superior approach for supercomputing-scale clusters (thousands of cores). However, Haiku's current decentralized per-CPU runqueues and tiered stealing are highly optimized for modern high-end workstations and servers (up to 512+ cores) and currently avoid the fragmentation issues that can arise from strict cellular sharding.

## 3. Performance Summary

### Current BMQ-EEVDF Strengths:
1.  **High Precision**: 512 bins (versus QAD's 128) provide better equity for mixed workloads.
2.  **Proven Interactivity**: The interactivity score and Display-Priority awareness are fine-tuned for Haiku's UI responsiveness.
3.  **Boot-time Calibration**: Dynamic lag thresholds ensure optimal performance across varying hardware interconnects.
4.  **Decentralized Accounting**: Recent 2025 audits removed global lock contention on runnable counters and masks.

### Proposed QAD Strengths:
1.  **Algorithmic Simplicity**: The branchless bit-parallel radix path is elegant and extremely fast.
2.  **Massive Scale Ready**: Cellular sharding provides a roadmap for hardware that Haiku does not currently target (4096+ cores).
3.  **Unified Path**: The goal of a single, branchless execution path for all core counts is ideal for instruction throughput.

## 4. Conclusion and Recommendation

**Verdict: The current BMQ-EEVDF scheduler is superior for Haiku OS.**

While QAD offers interesting concepts for ultra-massive scalability, Haiku's current BMQ-EEVDF implementation is a more mature and feature-complete realization of similar principles. Specifically:
-   **Resolution**: BMQ-EEVDF has 4x the resolution (512 bins) while maintaining the same $O(1)$ complexity.
-   **Interactivity**: The existing interactivity score is more nuanced than the proposed QAD credits.
-   **Practical Scalability**: The 2025 audit resolved the primary bottlenecks QAD aimed to address (lock contention and mask limits), making Haiku's current scheduler performant on any hardware Haiku is likely to run on.

**Recommendation**: Retain the current BMQ-EEVDF scheduler. The QAD proposal confirms that Haiku's direction is correct, but many of the "improvements" suggested in QAD are already present or exceeded by the 2025 audited implementation. Future work should focus on Phase 4 Roadmap items like Hardware-Guided EAS rather than a fundamental architecture swap.
