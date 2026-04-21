# Kernel Scheduler Complexity Analysis

This document summarizes the time complexity of key scheduler algorithms, ranked from highest (worst-case) to lowest.

## 1. Topology-Aware Work Stealing
**Complexity:** `O(C + log P)` or `O(1)`
*Where `C` is the number of cores per package and `P` is the number of packages.*

When a core becomes idle, it attempts to steal work from other cores to maintain high utilization.
1.  **Local Package Scan:** Linearly scans all `C` cores in the local package. This is acceptable for small `C` (L3 domain).
2.  **Local Node Scan:** For small NUMA nodes, it uses a rotational linear scan. For larger nodes, it switches to **Random Sampling**, bounding the cost.
3.  **Global Random Search:** Probes a fixed number of random packages across the entire system.

This implementation ensures that the work-stealing overhead does not scale linearly with system size, enabling efficient operation on many-core systems (up to 4096 cores).

## 2. Linear Core Selection (Small Clusters)
**Complexity:** `O(C)`
*Where `C` is the number of cores in the package.*

**Condition:** `PackageCoreCount < kRandomSearchThreshold` (Default: 8)

For packages with few cores, the scheduler performs a full linear scan of all cores to find the one with the absolute minimum load. This guarantees optimal placement but incurs linear cost, which is acceptable for small `C`.

## 3. Probabilistic Core Selection (Large Clusters)
**Complexity:** `O(1)` (Constant Time)

**Condition:** `PackageCoreCount >= kRandomSearchThreshold` (Default: 8)

For packages with many cores, a linear scan causes excessive cache pollution and lock contention. The scheduler switches to a **"Power of Two Choices"** algorithm:
1.  Samples a fixed number of random cores (4 attempts).
2.  Selects the candidate with the lowest load.

This ensures the cost of core selection remains constant regardless of how many cores are in the system.

## 4. Run Queue Operations (Enqueue/Dequeue/Peek)
**Complexity:** `O(1)` (Constant Time)

The scheduler uses a priority-based run queue implemented as an array of doubly-linked lists (one per priority level) and a bitmap for quick lookup.

*   **Enqueue/Dequeue:** constant time pointer updates.
*   **PeekMaximum:** Uses `fls` (find last set bit) on the bitmap to find the highest priority level in `O(1)`.
*   **PeekBest:** Scans up to 3 non-empty priority levels to find the best thread (lowest virtual runtime). This scan is strictly bounded by `kDeadlineLookaheadLevels` * `kSearchDepth` (3 * 32), making the operation `O(1)` in the worst case.

## 5. Team Enumeration
**Complexity:** `O(1)` (Amortized)

Iterating through active teams (e.g., for system info) uses the global team hash table iterator. This avoids the previous `O(MaxID)` scan of the entire ID space and provides constant amortized time access to the next active team.
