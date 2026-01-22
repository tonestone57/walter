# Estimated Performance Improvements for Future Scheduler Tasks

This document provides estimated performance gains for the suggested future improvements to the Haiku scheduler. Note that "Performance" can refer to throughput (work done), latency (responsiveness), or power efficiency depending on the task.

| Task | Throughput (Sys) | Latency (Resp) | Power Efficiency | Complexity |
| :--- | :---: | :---: | :---: | :---: |
| **1. Heterogeneous (big.LITTLE) Support** | +5-15% | +10% | **+25-40%** | High |
| **2. Advanced NUMA / Recursive Domains** | +10-20% | +5% | +5% | Very High |
| **3. Adaptive Interrupt Coalescing (IPI)** | +3-5% | -2% (Trade-off) | +5-10% | Medium |
| **4. Per-Entity Load Tracking (PELT)** | +0-2% | +5-10% | +5% | High |
| **5. Deadline Scheduling (EDF)** | 0% | **+100% (RT)** | 0% | High |
| **6. Lock-Free RunQueues** | +5-8% | +5% | +2% | Extreme |
| **7. Dynamic Power Integration** | -2% | +5% | **+15-30%** | Medium |

---

## Detailed Breakdown

### 1. Heterogeneous Architecture Support (big.LITTLE / Hybrid)
*   **Performance (+5-15%):** Moving background tasks to E-cores frees up P-cores for compute-heavy threads, effectively increasing the "useful" capacity of the biggest cores.
*   **Power (+25-40%):** Background services (daemons, indexing) consume significantly less power on E-cores. This is the primary driver for modern laptop battery life.

### 2. Advanced NUMA / Recursive Domains
*   **Throughput (+10-20%):** On large servers (4+ sockets), keeping memory access local (L3/RAM) is the single biggest performance factor. Reducing remote memory access by 50% can boost bandwidth-bound apps by 20%+.
*   **Context:** Only relevant for large multisocket systems (e.g., Threadripper, EPYC, Xeon SP).

### 3. Adaptive Interrupt Coalescing (Lazy IPI)
*   **Throughput (+3-5%):** Sending an IPI forces the target CPU to flush its pipeline and run an ISR. Avoiding this on 10,000+ interrupts/sec workloads saves significant overhead.
*   **Latency (-2%):** Trade-off. A thread might sit in the runqueue for an extra 100-500µs waiting for the next tick, slightly hurting instantaneous responsiveness.

### 4. Per-Entity Load Tracking (PELT)
*   **Latency (+5-10%):** By "knowing" a task is heavy immediately upon wake-up (history), the scheduler can place it on an empty core instantly, rather than placing it on a busy core and migrating it later.
*   **Responsiveness:** Reduces "micro-stutter" when launching heavy applications.

### 5. Deadline Scheduling (EDF)
*   **Latency (RT):** For audio/video threads, this prevents glitches entirely under load. It doesn't make code run faster, but it guarantees *timeliness*, effectively boosting "real-time performance" to 100% reliability.

### 6. Lock-Free RunQueues
*   **Throughput (+5-8%):** Eliminates the spinlock wait time on the RunQueue. Critical for systems with extreme context switch rates (e.g., high-IO web servers, database engines).
*   **Note:** Extremely difficult to implement correctly without introducing subtle race conditions.

### 7. Dynamic Power Integration (Schedutil)
*   **Power (+15-30%):** Racing to sleep (running fast then idling) vs running slow and steady. Intelligent frequency selection based on load prediction prevents the CPU from running at 5GHz for a background update.
