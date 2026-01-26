# Scheduler Algorithm Analysis

## Work Stealing Complexity
Is Work Stealing O(1)? **No, not strictly in the current implementation.**

### Current Behavior (Hybrid)
1.  **Local Package:** Linear scan of all cores (`O(C)`).
2.  **Remote Packages:** Iterates through **all** busy packages in the node using a bitmask (`O(P_busy)`).
3.  **Victim Core:** Once a package is selected, it uses **Random Sampling** (max 4 attempts) to find a core (`O(1)`).

**Total Complexity:** `O(C + P_busy)`

### Reducing to O(1)
To make work stealing strictly `O(1)`, we must eliminate the loops:
1.  **Random Package Selection:** Instead of iterating `busyPackageMask` with `ctz`, we must **randomly sample** the mask (e.g., pick a random bit index and scan forward to the next set bit).
2.  **Random Local Core:** Instead of scanning all local cores, we would randomly probe them (similar to the remote logic).

**Bitmasks alone do not make it O(1).**
*   `ctz` (Count Trailing Zeros) finds the *first* set bit in `O(1)` CPU cycles.
*   However, if you loop `while (mask != 0) { ... mask &= ~(1<<bit); }`, you are still performing `N` iterations. This is `O(N)`, not `O(1)`.

---

## Scheduling Policy (Vruntime vs Priority)
The scheduler uses a **Strict Priority + Vruntime** hybrid model.

1.  **Primary Key: Priority**
    *   The scheduler maintains 128 distinct priority queues (bitmap + array of lists).
    *   It **always** selects the highest priority queue that is non-empty. A thread with priority 20 will *always* preempt a thread with priority 19, regardless of vruntime.

2.  **Secondary Key: Virtual Runtime (Vruntime)**
    *   **Within the same priority level**, threads are ordered by `vruntime` (lowest first).
    *   `PeekBest` scans the head of the highest-priority queue (up to `kSearchDepth` = 32) to find the thread with the minimum `vruntime`.

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
