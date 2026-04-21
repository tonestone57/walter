# Scheduler Algorithm Analysis

## Work Stealing Complexity
Is Work Stealing O(1)? **No, not strictly in the current implementation.**

### Current Behavior (Optimized Hybrid)
1.  **Local Package:** Linear scan of all cores in the same package (`O(C)`). This is efficient for typical L3 domains (2-16 cores).
2.  **Local Node:** Uses **Rotational Linear Scan** for small nodes (<= 8 packages) or **Random Sampling** for larger nodes. This bounds the search cost.
3.  **Global Search:** Uses **Random Sampling** across all packages in the system. The number of samples is dynamically scaled based on system size (`gRandomSamples`), ensuring `O(1)` complexity relative to system size.
4.  **Victim Selection:** Within a selected package, it samples cores (Power of Two Choices) if the package is large, or performs a linear scan for small packages.

**Total Complexity:** `O(C + log P)` or `O(1)` depending on node size and sampling parameters. The implementation avoids the `O(N)` scan of all packages.

**Bitmasks and Sampling:**
*   `ctz` (Count Trailing Zeros) finds the *first* set bit in `O(1)` CPU cycles.
*   Hierarchical idle bitmasks (`gIdleNodeMask`, `fIdlePackageMask`) allow instant discovery of idle resources.
*   Random sampling prevents lock contention and cache line bouncing on large systems.

---

## Scheduling Policy (Vruntime vs Priority)
The scheduler uses a **Strict Priority + Vruntime** hybrid model.

1.  **Primary Key: Priority**
    *   The scheduler maintains 128 distinct priority queues (bitmap + array of lists).
    *   It **always** selects the highest priority queue that is non-empty. A thread with priority 20 will *always* preempt a thread with priority 19, regardless of vruntime.

2.  **Secondary Key: Virtual Runtime (Vruntime)**
    *   **Within priority levels**, threads are ordered by `vruntime` (lowest first).
    *   `PeekBest` scans up to 3 non-empty priority levels (highest first) and finds the best candidate across them within a search depth of 32 per level. This allows threads with significantly earlier deadlines in slightly lower priority buckets to be considered.

---

## Realtime and Display Threads

### Realtime Threads (`priority >= 100`)
*   **Policy:** **FIFO (First-In, First-Out)**.
*   **Vruntime Ignored:** The `ThreadDataVRuntimeCompare` function explicitly skips vruntime checks for realtime threads: `if (a->IsRealTime()) return false;`.
*   They run until they block, yield, or are preempted by a higher-priority realtime thread.

### Display Threads (`B_DISPLAY_PRIORITY` = 15, `B_URGENT_DISPLAY` = 20)
*   **Policy:** Treated as normal threads (Vruntime-managed).
*   **Heuristic:** They receive a massive **Vruntime Bonus** during activity accounting.
    *   In `UpdateActivity`: `fVirtualRuntime += (active * B_URGENT_DISPLAY_PRIORITY) / priority;`
    *   This formula scales the vruntime penalty. Since display threads have higher priority (15/20) than normal (10), their vruntime grows *slower* than normal threads for the same CPU usage, effectively giving them a larger share of the CPU bandwidth before they are preempted.
