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

### E. Cache Efficiency and L1 Impact
- **BMQ-EEVDF (Current)**:
    - **Selection Metadata**: Spans ~136 bytes (`fFirstLevelBitmap` + 16 words of `fSecondLevelBitmap`).
    - **Cache Footprint**: Typically spans **3 cache lines** (64-byte each).
    - **Access Overhead**: Requires two sequential memory loads to identify the target bin (FLI then SLI).
- **QAD (Proposed)**:
    - **Selection Metadata**: Consists of a flat 128-bit mask (16 bytes).
    - **Cache Footprint**: Fits entirely within **1/4 of a single cache line**.
    - **Access Overhead**: Potentially reduced to a **single memory load** (or two if the first 64 bits are empty).

**Analysis**: QAD is significantly more L1 cache-efficient than the current 16x32 matrix. The 16-byte footprint ensures that scheduling metadata is much more likely to remain "hot" in the L1 cache, reducing stalls during context switches. However, this efficiency comes at the cost of **4x lower resolution** compared to Haiku's current 512-bin structure.

### F. Scaling QAD to 512 Lanes (Apples-to-Apples)
If QAD were extended to 512 lanes to match BMQ-EEVDF's resolution, the comparison shifts:

- **QAD-512**:
    - **Selection Metadata**: 64 bytes (8x `uint64`).
    - **Cache Footprint**: Fits into **exactly one cache line**.
    - **Access Path**: Requires 1-8 sequential loads. However, because all 8 words reside in the same cache line, the probability of a second cache miss is near zero.
- **BMQ-EEVDF (Current)**:
    - **Selection Metadata**: ~136 bytes.
    - **Cache Footprint**: Spans **3 cache lines**.
    - **Access Path**: Always requires 2 sequential loads (FLI then SLI). If the SLI word is in a different cache line (likely), it triggers a second L1/L2 access stall.

**Analysis**: At equivalent resolutions, the flat QAD-style bitmask is **technically superior** for L1 efficiency. It consolidates all selection logic into a single cache line (64 bytes) and eliminates the two-level dependent load hierarchy. While BMQ-EEVDF was a brilliant optimization for 32-bit systems with limited registers, a modern flat bitmask is more performant on 64-bit architectures.

## 3. Performance Summary

### Current BMQ-EEVDF Strengths:
1.  **High Precision**: 512 bins (versus QAD's 128) provide better equity for mixed workloads.
2.  **Proven Interactivity**: The interactivity score and Display-Priority awareness are fine-tuned for Haiku's UI responsiveness.
3.  **Boot-time Calibration**: Dynamic lag thresholds ensure optimal performance across varying hardware interconnects.
4.  **Decentralized Accounting**: The implementation uses per-CPU counters to avoid global lock contention on runnable threads.

### Proposed QAD Strengths:
1.  **Algorithmic Simplicity**: The branchless bit-parallel radix path is elegant and extremely fast.
2.  **L1 Cache Efficiency**: The 16-byte bitmask footprint is exceptionally dense, fitting selection metadata into a fraction of a single cache line.
3.  **Massive Scale Ready**: Cellular sharding provides a roadmap for hardware that Haiku does not currently target (4096+ cores).
4.  **Unified Path**: The goal of a single, branchless execution path for all core counts is ideal for instruction throughput.

## 4. Conclusion and Recommendation

**Verdict: The current BMQ-EEVDF scheduler is superior for Haiku OS.**

While QAD offers interesting concepts for ultra-massive scalability, Haiku's current BMQ-EEVDF implementation is a more mature and feature-complete realization of similar principles. Specifically:
-   **Resolution**: BMQ-EEVDF has 4x the resolution (512 bins) while maintaining the same $O(1)$ complexity.
-   **Interactivity**: The existing interactivity score is more nuanced than the proposed QAD credits.
-   **Cache vs. Resolution Tradeoff**: While QAD is more L1 cache-efficient, BMQ-EEVDF's 512-bin matrix provides the fine-grained deadline tracking necessary for Haiku's diverse desktop workloads. The current metadata size (~136 bytes) is a reasonable trade-off for the 4x increase in scheduling precision.
-   **Practical Scalability**: The scheduler resolves primary scalability bottlenecks (lock contention and mask limits) through decentralized accounting and bitset-based idle masks, making it performant on high core-count systems.

**Recommendation**: Retain the mature BMQ-EEVDF core logic but modernize it using QAD's flat bitmask and explicit scaling techniques. The QAD proposal confirms that Haiku's direction is correct, but the existing interactivity scoring is more nuanced than basic credits.

## 5. Implemented Optimizations Derived from QAD

The following optimizations, inspired by the QAD proposal, have been implemented to modernize the current BMQ-EEVDF scheduler:

### 1. Flat 512-Lane Bitmask Migration
The 16x32 matrix structure has been replaced with a flat **512-lane bitmask**.
-   **L1 Cache Efficiency**: Selection metadata is now consolidated into **exactly one 64-byte cache line**. This ensures that the scheduling state remains "hot" and avoids multiple cache-line fills during context switches.
-   **Branchless Selection Path**: By using **inverted SLI mapping** (`lane = fli * 32 + (31 - sli)`), the scheduler ensures that higher bit indices always represent higher priority. This simplifies selection to a single word-level bit-scan (`scheduler_flsnative`), eliminating branches and dependent loads.
-   **Memory Footprint**: Reduced the per-CPU metadata footprint from ~136 bytes to 64 bytes (2.1x reduction).
-   **32-bit Portability**: On 32-bit architectures, the bitmask is unrolled into 16x32-bit words, allowing efficient scanning using native assembly primitives without 64-bit emulation overhead.

### 2. Capacity-Aware Virtual Runtime Scaling
Adopted QAD's direct heterogeneous scaling math for virtual runtime accumulation.
-   **Heterogeneous Fairness**: Wall-time is now explicitly scaled by the physical core's capacity rating, ensuring that threads running on Efficiency cores accumulate virtual runtime faster than those on Performance cores.
-   **32-bit Optimization**: To avoid expensive 64-bit integer division in the context-switch hot path, the scaling uses a **fixed-point score factor**. This allows 32-bit CPUs to perform accurate performance-scaling using only multiplication and bit-shifting.

## 6. Future Roadmap: Cellular Sharding
For systems targeting the "Enterprise Supercomputer" scale (1024+ cores), Haiku should continue to evaluate QAD's **Cellular Autonomy** strategy. Splitting the system into independent 32-core scheduling cells would eliminate the $O(N)$ overhead of scanning system-wide bitsets and counters, ensuring that context switch latency remains constant even at extreme core counts.
