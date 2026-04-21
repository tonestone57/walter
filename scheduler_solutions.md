# Scheduler Solutions & Improvements

This document outlines technical proposals to address the weaknesses identified in the scheduler comparison (Current Haiku Scheduler vs. Hybrid Design).

## 1. Addressing Weaknesses in the Current Method (Strict Priority)

### Problem: Starvation
**Analysis:** Low-priority threads depend entirely on high-priority threads yielding or blocking. The current "Priority Boosting" mechanism is *reactive*—it waits for a thread to be starved for a specific interval (`kPriorityBoostInterval`) before helping it.
**Proposed Solutions:**
*   **Proactive Stochastic Boosting:** Instead of waiting for starvation, occasionally inject "random noise" into the scheduler's decision-making or grant "lottery tickets" to low-priority threads, ensuring they get *some* CPU time statistically regardless of high-priority load.
*   **Borrowed Timeslices:** Allow low-priority threads to "borrow" a small quantum from a high-priority thread if they haven't run for a long time, repaying it later (lowering their priority temporarily).

### Problem: Deadlock Risk (Priority Inversion)
**Analysis:** The current `mutex` implementation (in non-debug mode) does not explicitly track the lock holder in the fast path to save space/cycles. Without knowing the holder, the kernel cannot boost the holder's priority when a high-priority thread blocks on it.
**Proposed Solutions:**
*   **Implement Priority Inheritance (PI):**
    *   *Mechanism:* Modify `mutex` to always store the `owner_thread_id`. When `Thread A` (High Prio) blocks on `Mutex M`, the kernel checks `M->owner`. If `M->owner` (Thread B) has a lower priority, boost `Thread B` to `Thread A`'s priority.
    *   *Trade-off:* Increases the size of the mutex structure and adds overhead to every lock/unlock operation.
*   **Priority Ceiling Protocol:** Assign a "Ceiling Priority" to specific mutexes used by real-time tasks. Any thread holding such a mutex immediately runs at the Ceiling Priority, preventing it from being preempted by medium-priority tasks.

---

## 2. Addressing Weaknesses in the Hybrid Method (Weighted Vruntime)

### Problem: Latency (Scheduling Overhead)
**Analysis:** The Hybrid design uses a Red-Black Tree. Inserting and removing threads is `O(log N)`. While `log(1000)` is small (~10), it is not `O(1)`.
**Proposed Solutions:**
*   **O(1) Min-Node Caching:**
    *   *Mechanism:* The scheduler maintains a pointer to the *leftmost* node (minimum vruntime) of the tree.
    *   *Result:* `PeekNextThread()` becomes **O(1)**. Only `Enqueue()` and `Dequeue()` remain `O(log N)`. This makes the critical dispatch path instant.
*   **Per-CPU Red-Black Trees:**
    *   *Mechanism:* Instead of one global tree (which requires a global lock), maintain a Red-Black Tree *per CPU*.
    *   *Result:* Eliminates global lock contention. Load balancing is handled by moving nodes between trees (work stealing).

### Problem: "Sluggish" Feel (Responsiveness)
**Analysis:** Fair schedulers can feel less "snappy" than strict priority schedulers because they wait for interactive tasks to accumulate "lag" (virtual time debt).
**Proposed Solutions:**
*   **The Latency Bonus (Turbocharger):**
    *   *Mechanism:* As proposed in the design, explicitly detect "interactive" events (mouse, keyboard, audio interrupts).
    *   *Action:* When waking a thread associated with these events, artificially subtract a significant amount from its `vruntime` (e.g., `vruntime -= latency_bonus`). This places it at the *far left* of the tree, guaranteeing immediate preemption of even high-weight batch tasks.
*   **Dual-Structure Scheduling:**
    *   *Mechanism:* Keep the **O(1) Bitmap** for Real-Time threads (Priority 100+) and use the **RB-Tree** for Normal threads (1-99).
    *   *Result:* Real-time audio/video tasks get the absolute zero-latency guarantee of the bitmap, while desktop applications enjoy the fairness of the tree.

---

## 3. Recommended Roadmap

1.  **Implemented (Responsiveness & Starvation):**
    *   **Proactive Boosting:** Updated `_ComputeEffectivePriority` to start boosting threads slightly earlier (75% of interval) to prevent edge-case starvation.
    *   **Latency Bonus:** Increased the vruntime "head start" for waking threads (from 2ms to 5ms) to improve interactive responsiveness (Hybrid Feature).
    *   **Multi-level search:** Updated `RunQueue::PeekBest` to search up to 3 non-empty priority levels, allowing threads with significantly earlier deadlines in lower priority buckets to preempt (Issue 40).

2.  **Future Work (Deadlock Mitigation):**
    *   **Priority Inheritance:** Requires modifying the `mutex` structure (ABI change) to store the owner thread ID. This is critical for preventing priority inversion but requires a synchronized kernel/driver rebuild.

3.  **Long Term (The Hybrid):**
    *   Transition to the full **Hybrid Scheduler** (RB-Tree + Latency Bonus) once the architectural prerequisites (like O(1) Min-Node caching) are ready.
