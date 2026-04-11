# Roadmap: Optimizing Haiku VD for Desktop Excellence

This document outlines architectural improvements to the Haiku Virtual Deadline (VD) scheduler to establish it as the premier desktop scheduler, focusing on ultra-low latency and superior interactive responsiveness.

## 1. Sub-Frame Preemption (1ms Deadline Resolution)

**Current State:** The scheduler uses a 5ms bucket size (`kDeadlineBucketSize`) for mapping virtual deadlines to dynamic priorities.
**Problem:** A 5ms resolution is too coarse for modern high-refresh-rate displays. At 144Hz, a frame is only ~6.9ms. If an interactive thread is misaligned by a 5ms bucket, it can easily miss its frame budget.
**Improvement:** Reduce `kDeadlineBucketSize` to **1ms**.
*   **Impact:** Provides 5x finer granularity for task urgency, ensuring that UI threads are prioritized with sub-frame precision.

## 2. Foreground Urgency Injection

**Current State:** The scheduler treats all interactive threads (priority > 10) similarly based on their sleep patterns.
**Problem:** The scheduler is "blind" to which application the user is currently interacting with. Background UI tasks (e.g., a music player updating a progress bar) can compete equally with the foreground window (e.g., a web browser scrolling).
**Improvement:** Leverage the `foreground_group` status from the kernel's `ProcessSession` to grant an "Urgency Bonus" to threads in the active process group.
*   **Mechanism:** Threads in the `foreground_group` receive a virtual runtime "head start" or a direct dynamic priority boost upon wake-up.
*   **Impact:** Guarantees that the app under the user's mouse/keyboard always feels "snappier" than any background activity.

## 3. Expanded Dynamic Priority Resolution

**Current State:** Internal priorities are mapped to a range of [0..120].
**Problem:** With many threads, multiple tasks often end up in the same "Urgency Bucket" (priority level) in the `RunQueue`, forcing the scheduler to use its bounded scan (depth 32) to break ties.
**Improvement:** Expand the dynamic priority range to **0..255**.
*   **Impact:** Reduces "bucket collisions" by over 50%, allowing the O(1) bitmap logic to find the *absolute* best candidate more frequently without scanning, further reducing dispatch jitter.

## 4. Adaptive Quantum Scaling (Display-Aware)

**Current State:** Timeslices are scaled primarily based on system load.
**Problem:** Interactive threads often need very short, frequent bursts rather than long quanta.
**Improvement:** Implement context-aware quantum scaling.
*   **Mechanism:** Detect "bursty" threads (high wake-up frequency, low CPU usage per run) and assign them shorter, high-urgency quanta. Conversely, detect batch threads and give them longer quanta to improve cache locality.
*   **Impact:** Maximizes throughput for background tasks while minimizing the "preemption lag" for UI updates.

## 5. Summary of Projected Benefits

| Improvement | Primary Metric | Expected Outcome |
| :--- | :--- | :--- |
| **1ms Resolution** | Dispatch Latency | Consistent frame-perfect UI updates at 144Hz+. |
| **Foreground Injection** | Perceived Speed | Zero-lag interaction even under heavy 100% CPU load. |
| **255 Priority Levels** | Jitter (Tail Latency) | Near-zero variance in context switch times. |
| **Adaptive Quanta** | Throughput | 2-5% improvement in IPC due to better cache retention for batch tasks. |

---

## Conclusion

By transitioning from a general-purpose O(1) scheduler to a **Context-Aware Virtual Deadline** model, Haiku can provide a level of desktop responsiveness that surpasses both the mathematical fairness of Linux EEVDF and the simplified efficiency of Redox DWRR.
