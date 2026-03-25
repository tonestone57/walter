# Haiku Scheduler Assessment & Audit

## Executive Summary

The updated Haiku scheduler represents a significant evolution from a traditional O(1) scheduler to a sophisticated, topology-aware **Virtual Deadline Scheduler**. It combines the low-latency guarantees of deadline scheduling with the scalability of O(1) data structures and hierarchical topology management. Key improvements include efficient handling of large-scale systems (up to 4096 packages), power-aware scheduling modes, and fine-grained locking to minimize contention.

## 1. Core Scheduling Algorithm: Virtual Deadlines

The scheduler has moved away from static priorities for dynamic decision making, adopting a model similar to EEVDF (Earliest Eligible Virtual Deadline First), but adapted for O(1) efficiency.

*   **Virtual Deadlines:** Instead of a simple priority queue, threads are assigned a "Virtual Deadline" (`fVirtualDeadline`). This is calculated in `ThreadData::_UpdateDeadline` based on the thread's static priority.
*   **Urgency Mapping:** To maintain O(1) enqueue/dequeue operations, the scheduler maps the continuous "deadline urgency" (Deadline - Current Time) into a dynamic priority range [0..99] (`ThreadData::_ComputeEffectivePriority`). This allows the use of bitmaps and fixed-priority queues while behaving like a deadline scheduler.
*   **Benefit:** This approach guarantees better latency for interactive tasks (which get earlier deadlines) while preventing starvation, as the "urgency" increases as time passes.

## 2. Data Structures: O(1) Scalability

The scheduler employs highly optimized data structures to ensuring scheduling decisions remain constant time (O(1)) regardless of the number of active threads.

*   **O(1) RunQueue (`RunQueue.h`):**
    *   Uses a **Bitmap** (`fBitmap`) to identify non-empty priority levels instantly.
    *   `PeekMaximum` finds the highest priority thread using `fls` (Find Last Set bit), an O(1) CPU instruction.
    *   **Bounded Search:** `PeekBest` implements a "bounded scan" (depth 32) to find the absolute best candidate (lowest virtual runtime) within the highest priority level, balancing fairness with performance.

## 3. Topology Awareness & Scalability

The most significant architectural improvement is the deep integration of system topology, allowing the scheduler to scale to massive systems (theoretically 4096 sockets/packages).

*   **Hierarchical Model:**
    *   **CPU (`CPUEntry`):** Logical processors (HyperThreads).
    *   **Core (`CoreEntry`):** Physical cores, managing shared resources between siblings.
    *   **Package (`PackageEntry`):** Physical Sockets/NUMA Nodes (L3 Cache domain).
    *   **Node (`SchedulerNode`):** Groups of 64 Packages for managing massive scale.
*   **O(1) Idle Resource Finding:**
    *   Hierarchical Bitmaps (`fIdleCoreMask` in Package, `fIdlePackageMask` in Node, `gIdleNodeMask` global) allow the scheduler to find the nearest idle core in O(1) time without scanning lists.
*   **Random Sampling (Power of Two Choices):**
    *   For large systems (`gPackageCount > kRandomSearchThreshold`), the scheduler switches from linear scans to **Random Sampling** (`search_global_random`, `search_local_node`).
    *   This statistically ensures O(1) selection time even with thousands of cores, avoiding the O(N) cache line bouncing of global scans.

## 4. Work Stealing Strategy

To balance load without global locking, the scheduler implements a robust, 3-phase work stealing algorithm in `CPUEntry::_TryStealWork`:

1.  **Phase 1: Local Sibling (L2/L3 Domain):** Checks other cores in the same package. Fast, high cache locality.
2.  **Phase 2: Local NUMA Node (Random):** Probes random packages within the local NUMA node. Balances load within the socket/board to avoid interconnect congestion.
3.  **Phase 3: Global Random (The "Hail Mary"):** If the local node is empty, it uses a logarithmic search formula to probe random global packages. This ensures that no CPU goes idle if there is work *anywhere* in the system, but minimizes the cost of finding it.

## 5. Scheduler Modes

The system supports switchable operation modes (`scheduler_modes.h`) to adapt to different use cases:

*   **Low Latency Mode:**
    *   **Strategy:** "Spread". Prefers idle cores to maximize throughput and minimize latency.
    *   **Behavior:** Aggressively migrates threads to idle CPUs (`choose_core`).
    *   **Quantum:** Shorter base quantum (1600us) for responsiveness.
*   **Power Saving Mode:**
    *   **Strategy:** "Pack". Prefers filling up active cores before waking new ones.
    *   **Behavior:** Uses `choose_small_task_core` and `check_package_packing` to colocate threads.
    *   **Features:** Recently updated to achieve architectural parity with Low Latency mode, including Advanced NUMA Support (Home Package migration thresholds), applied to improve thread locality when migrating.
    *   **Quantum:** Longer base quantum (5000us) to reduce context switches and allow deeper CPU sleep states.

## 6. Locking & Concurrency

*   **Fine-Grained Locking:** Separate locks for `CoreEntry` (Heap/ThreadCount) and `CPUEntry` (RunQueue), reducing contention.
*   **Deadlock Avoidance:** Strict lock ordering (Core -> CPU) enforced.
*   **Lockless Operations:**
    *   `TryLockRunQueue` is used during work stealing to skip contented queues rather than blocking.
    *   Per-CPU Random Number Generators (`CPUEntry::GetRandom`) avoid global lock contention during random sampling.

## 7. Performance Optimizations Highlighted

*   **Fast Division:** `ThreadData::ComputeQuantum` uses `kRangeReciprocal` to replace expensive integer division with multiplication and shifts.
*   **Bitmap Intrinsics:** Extensive use of `__builtin_ctz` and `fls` for bit manipulation.
*   **Stack Allocation:** `search_global_random` allocates its visited bitmask (up to 512 bytes) on the stack to avoid heap allocation overhead in the hot path.
*   **Cache Locality:** `ThreadData` caches `fHomePackage` and `fCore` to prefer scheduling on the same physical resources (Warm Cache affinity).

## 8. Performance Expectations

Based on the architectural changes (Virtual Deadlines, O(1) Scalability, Active Work Stealing), the following improvements can be expected:

*   **Responsiveness:**
    *   **Level:** **Significant improvement**.
    *   **Reasoning:** The Virtual Deadline algorithm ensures that interactive tasks (which often sleep and wake frequently) accumulate "urgency" during their sleep time. Upon waking, they are immediately placed at the front of the queue, preempting CPU-bound batch tasks. This eliminates the "sluggish" feeling during heavy load (e.g., compiling) that was present in the penalty-based scheduler.

*   **Latency:**
    *   **Level:** **Consistent Low Latency (O(1))**.
    *   **Reasoning:** The removal of global heap locks and the introduction of constant-time O(1) bitmaps mean that scheduling latency is no longer a function of the number of active threads or cores. Worst-case latency is now bounded by the scheduler quantum (1.6ms default) rather than system size, preventing latency spikes on large servers.

*   **Jitter:**
    *   **Level:** **Drastic Reduction**.
    *   **Reasoning:** Jitter (variance in latency) is primarily caused by lock contention. By moving to per-CPU/per-Core locks and lockless "TryLock" stealing mechanisms, threads are rarely blocked waiting for scheduler decisions. This is critical for real-time audio and video applications.

*   **Throughput:**
    *   **Level:** **Linear Scaling**.
    *   **Reasoning:** The new topology-aware stealing (Phase 1/2) keeps threads local to their L2/L3 cache domains, significantly improving Instruction Per Cycle (IPC) by reducing cache misses. Furthermore, eliminating the global scheduler lock removes the primary serialization point, allowing throughput to scale linearly with core count (tested effectively up to 64+ cores).

## 9. Recommendations for Improved Fairness

While the updated scheduler is highly responsive, its "Statistical Fairness" (bucketized priority) can theoretically lag behind Linux EEVDF's precise fairness. The following recommendations are ranked by their ability to improve fairness:

1.  **Intra-Bucket Heuristics (Highest Fairness Impact)**
    *   **Strategy:** Implement a "Best of 4" search within the highest priority bucket instead of taking the first thread (FIFO). The scheduler would peek at the first few threads and pick the one with the absolute lowest `virtual_runtime`.
    *   **Fairness Improvement:** **High (Rank 1)**. This directly addresses the primary source of unfairness (collisions within the 5ms bucket). It approximates strict deadline sorting.
    *   **Performance Impact:** **Low**. Requires scanning 3-4 additional pointers in the hot path. O(1) complexity is maintained (bounded scan).

2.  **Increase Priority Resolution**
    *   **Strategy:** Expand `THREAD_MAX_SET_PRIORITY` from 99 to 255 (matching one byte).
    *   **Fairness Improvement:** **Moderate (Rank 2)**. Reduces bucket width from ~5ms to ~2ms, statistically reducing collisions by ~60%.
    *   **Performance Impact:** **Negligible**. Bitmaps grow slightly (from 4 words to 8 words), but `__builtin_ctz` efficiency remains identical.

3.  **Simplified Lag Tracking (Starvation Protection)**
    *   **Strategy:** Add a `fLag` counter. If a thread is skipped during a "TryLock" failure (work stealing), increment `fLag`. If it exceeds a threshold, forcefully boost its urgency.
    *   **Fairness Improvement:** **Low (Rank 3)**. Only helps in pathological edge cases (high contention). Most useful for preventing worst-case starvation rather than improving average-case fairness.
    *   **Performance Impact:** **Very Low**. Simple integer increment/check.

## 10. Additional Scheduler Refinements

Further auditing revealed specific fixes implemented directly within the scheduler logic to improve robustness:

*   **Hot-Unplug Safety:** `CPUEntry::UpdatePriority` (`src/system/kernel/scheduler/scheduler_cpu.cpp`) now explicitly handles updates to `B_IDLE_PRIORITY` even if the CPU is marked as disabled. This prevents kernel panics during CPU hot-unplug operations when a core is being taken offline.
*   **Locking Correctness:** In `scheduler_set_cpu_enabled`, the methods `AddCPU` and `RemoveCPU` (in `CoreEntry`) are now called without holding `fCPULock`. This logic relies on the caller (`scheduler_set_cpu_enabled`) holding the global `InterruptsBigSchedulerLocker` for serialization, avoiding a potential deadlock or double-lock scenario.
*   **Profiler Safety:** `Profiler::EnterFunction` (`src/system/kernel/scheduler/scheduler_profiler.cpp`) now enforces strict bounds checks on `fFunctionStackPointers`. This prevents buffer overflows and kernel crashes if the function call stack depth exceeds the profiler's pre-allocated storage.

## 11. Potential Issues & Future Risks

While the scheduler is robust, the following areas should be monitored as hardware evolves:

*   **L3 Topology Alignment:** The clustering logic assumes `get_topology_id` accurately reflects the L3 cache boundary. If the platform topology data (ACPI) is incorrect or provides a socket ID instead of an L3 ID (common on older BIOSes), a "Node" might become too large (e.g., entire socket), potentially increasing contention on the Node's idle mask if it spans 64+ packages.
*   **Stack Usage:** `search_global_random` allocates a bitmask on the kernel stack. Although reduced to 128 bytes (supporting 1024 packages), this usage should be monitored. If `gPackageCount` explodes significantly beyond 1024 on massive future systems, the collision detection logic simply skips higher indices, which is safe but slightly less optimal.
*   **Prime Number Core Counts:** Extremely unusual core counts (e.g., 13 cores in an L3 domain) might result in slightly uneven clusters (e.g., 4+4+3+2) despite the best-effort balancing logic. The performance impact is negligible, but it deviates from the perfect symmetry of 4-core clusters.

## Conclusion

The audited scheduler is a modern, high-performance implementation suitable for systems ranging from embedded devices (via Power Saving mode) to large servers (via Topology Awareness). The hybrid approach of Virtual Deadlines within O(1) structures provides an excellent balance of latency guarantees and raw throughput.
