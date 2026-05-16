# Comparison: Current Virtual Deadline vs. EEVDF

Haiku's current scheduler (2025 Audit version) uses virtual deadlines, but its implementation differs fundamentally from the **EEVDF (Earliest Eligible Virtual Deadline First)** algorithm. This report outlines the technical improvements EEVDF would provide.

## 1. Current Implementation: "Deadline-to-Priority" Mapping

The current scheduler is a **Priority-Based Preemptive Scheduler** that uses deadlines as a heuristic for urgency.

- **Mechanism:** `_UpdateDeadline()` calculates a future timestamp based on priority and interactivity. `_ComputeEffectivePriority()` then maps the remaining time (`deadline - now`) into a discrete dynamic priority level (0-31).
- **Ordering:** The `RunQueue` is a multi-level priority queue. Threads in higher priority buckets always preempt or run before lower ones.
- **Role of Deadlines:** Deadlines are essentially a "sorting aid" within priority buckets or a way to bump a thread into a higher bucket (increasing its "urgency").

## 2. EEVDF: Proportional Share with Decoupled Latency

EEVDF is a **Fair-Queueing Scheduler** that replaces fixed priority levels with mathematical share and request bounds.

### 2.1 Decoupling Throughput from Latency
- **Current:** If an audio thread needs low latency (300µs), it must be assigned a high priority. This high priority also grants it a large slice of the CPU (high throughput), even if the thread only does a tiny amount of work.
- **EEVDF:** Latency and throughput are independent parameters. A thread can have a **low weight** (2% of the CPU) but a **short request** (500µs latency). This allows interactive background tasks to be responsive without starving other processes or using more than their fair share of the CPU.

### 2.2 Strict Eligibility & Lag Tracking
- **Current:** The scheduler uses an "interactivity score" to reward bursty threads. However, there is no strict tracking of how much service a thread was "owed" versus what it received.
- **EEVDF:** It explicitly tracks **Lag** (the difference between a thread's fair share and its actual service).
  - A thread only becomes **eligible** when its virtual runtime is $\le$ the system's virtual time.
  - This prevents "bursty" threads from unfairly dominating the CPU after waking up from a long sleep, as they are only scheduled once they are "owed" time.

### 2.3 Mathematical Fairness Bounds
- **Current:** Fairness is emergent but not guaranteed. High-priority threads can theoretically starve lower ones if the priority mapping is aggressive.
- **EEVDF:** It provides a proven bound where no thread lags more than one maximum request size behind its fair share. This makes it a "harder" real-time environment for professional media production.

---

## 3. Summary of Improvements

| Feature | Current Scheduler | EEVDF Optimization |
| :--- | :--- | :--- |
| **Primary Axis** | Priority Levels (0-31) | Virtual Time (Fairness) |
| **Latency Control** | High Priority = Low Latency | **Short Requests = Low Latency** |
| **Throughput Control** | High Priority = High Throughput | **Weight/Shares = High Throughput** |
| **Fairness** | Heuristic (Urgency Mapping) | **Mathematical (Lag Management)** |
| **Bursty Behavior** | Interactivity Score Bonus | **Eligibility Thresholds** |

## Conclusion
While the current scheduler's use of virtual deadlines significantly improves upon traditional round-robin or simple priority schemes, it remains bound by the discrete nature of priority levels. Transitioning to EEVDF would offer Haiku **deterministic Quality of Service (QoS)**, allowing the Media Kit and other interactive components to guarantee responsiveness without over-allocating CPU throughput to low-workload threads.
