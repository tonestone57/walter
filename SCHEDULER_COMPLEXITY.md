# Kernel Scheduler Complexity Analysis

This document summarizes the time complexity of key scheduler algorithms, ranked from highest (worst-case) to lowest.

## 1. Topology-Aware Work Stealing
**Complexity:** `O(P + C)`
*Where `P` is the number of packages in the NUMA node and `C` is the number of cores per package.*

When a core becomes idle, it attempts to steal work from other cores to maintain high utilization.
1.  **Local Package Scan:** Linearly scans all `C` cores in the local package.
2.  **Node Package Scan:** If local stealing fails, it iterates through sibling packages (`P`) in the same NUMA node.
3.  **Victim Selection:** For each busy sibling package, it attempts to steal from a random core (up to 4 attempts).

This is the most expensive operation in the scheduler's hot path, scaling with the complexity of the CPU topology.

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
*   **PeekBest:** Scans the highest priority list to find the thread with the lowest virtual runtime. This scan is strictly bounded by `kSearchDepth` (32), making the operation `O(1)` in the worst case.

## 5. Team Enumeration
**Complexity:** `O(1)` (Amortized)

Iterating through active teams (e.g., for system info) uses the global team hash table iterator. This avoids the previous `O(MaxID)` scan of the entire ID space and provides constant amortized time access to the next active team.
