# Haiku Scheduler Evaluation: BMQ-EEVDF vs. Quantized Asymmetric Deadline (QAD)

## 1. Overview
This report evaluates the current Haiku scheduler (BMQ-EEVDF) against the proposed Quantized Asymmetric Deadline (QAD) scheduler. The goal is to determine which architecture is better suited for Haiku OS, particularly regarding performance, scalability, and responsiveness.

## 2. Architectural Comparison

### A. Selection Complexity and Data Structures
| Feature | BMQ-EEVDF (Baseline) | QAD (Proposed) | BMQ-EEVDF (Modernized) |
| :--- | :--- | :--- | :--- |
| **Selection Speed** | $O(1)$ (16x32 bitmask matrix) | $O(1)$ (128-lane bitmask) | $O(1)$ (512-lane bitmask) |
| **Resolution** | 512 discrete bins per CPU | 128 discrete lanes per CPU | 512 discrete lanes per CPU |
| **Fairness** | Formal EEVDF (Lag-based) | Quantized Lag Fairness | Formal EEVDF (Lag-based) |
| **Complexity** | Hybrid: Bitmask + Doubly Linked Lists | Dual-Array Bitmask (Active/Expired) | Flat Bitmask + Doubly Linked Lists |
| **Cache Line Impact** | Spans 3 Cache Lines (~136 bytes) | Spans 1/4 Cache Line (16 bytes) | Fits in 1 Cache Line (64 bytes) |

**Analysis**: By migrating to a flat 512-lane bitmask, the modernized BMQ-EEVDF achieves the L1 cache efficiency of QAD while maintaining 4x the scheduling resolution.

### B. Heterogeneous Core Support (P vs. E Cores)
- **Baseline**: Used `fCapacity` and `fPerformanceScale` metrics.
- **QAD**: Scales virtual runtime accumulation by `(actual_duration * 1024) / core_capacity`.
- **Modernized**: Implements **Fixed-Point Capacity Scaling**. Pre-calculates a 32-bit `fScoreFactor` metric (`(kDefaultCapacity << 16) / fCapacity`) to perform performance scaling via multiplication and bit-shifting.

**Analysis**: The modernized implementation adopts QAD's direct scaling math but optimizes it for 32-bit architectures by eliminating 64-bit division in the context-switch hot path.

### C. Interactivity and Responsiveness
- **Baseline**: Employs an **Interactivity Score (0-1000)** and "Quick Start Credit" based on formal EEVDF math.
- **QAD**: Proposed **Interactive Latency Credits** which roll back virtual runtime upon wakeup.
- **Modernized**: Retains the nuanced interactivity score but integrates QAD-inspired **Scaled Lag Floors** and **Interactivity-Dependent Request Sizes** ($1\text{ms}$ for interactive, $5\text{ms}$ for batch) to ensure deterministic preemption.

**Analysis**: Integrating QAD's credit concepts into EEVDF's formal deadline math provides a smoother gradient of responsiveness than a simple rollback.

### D. Scalability and Work-Stealing
- **Baseline**: Hierarchical "Laggiest Wins" stealing across L3, NUMA, and Global domains.
- **QAD**: Proposes **Cellular Autonomy** (sharding into 32-core cells) for systems up to 4096 cores.
- **Modernized**: Uses **Decentralized Atomic Accounting** and bitset-based idle masks.

**Analysis**: While cellular sharding is superior for supercomputing scales, the current decentralized architecture is optimal for Haiku's target hardware (1-512 cores), avoiding the resource fragmentation of strict cells.

## 3. Performance Summary

### Modernized BMQ-EEVDF Strengths:
1.  **Selection Efficiency**: Consolidating selection metadata into a single 64-byte cache line reduces L1 access stalls.
2.  **High Precision**: 512-lane resolution provides precise deadline tracking for complex desktop workloads.
3.  **Branchless Path**: Inverted SLI mapping enables a branchless context-switch path.
4.  **Optimized Scaling**: Fixed-point math removes 64-bit division overhead on 32-bit and 64-bit systems.

## 4. Conclusion and Recommendation

**Verdict: The modernized BMQ-EEVDF scheduler is the superior choice for Haiku OS.**

By integrating the best features of the QAD proposal—specifically the **flat bitmask structure** and **explicit capacity scaling**—into the formal EEVDF framework, we have achieved a "best of both worlds" architecture.

-   **Resolution**: We maintain 4x the precision (512 lanes) of the original QAD proposal.
-   **Cache Density**: Metadata is 2.1x denser than the baseline, fitting selection logic into a single cache line.
-   **Responsiveness**: The formal EEVDF deadline math, augmented by interactivity-weighted request sizes, ensures the near-zero input latency Haiku is famous for.
-   **Hardware Portability**: The use of fixed-point scaling and native word-sized bit-scans makes the scheduler equally performant on legacy 32-bit hardware and modern 64-bit servers.

**Final Decision**: The scheduler has been successfully modernized using QAD-inspired optimizations. The architecture is now more L1-efficient, branchless, and asymmetric-aware while preserving the mathematical elegance of EEVDF.
