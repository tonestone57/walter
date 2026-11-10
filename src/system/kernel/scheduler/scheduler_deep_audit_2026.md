# Haiku Kernel Scheduler In-Depth Technical Analysis and Audit (2026)

## 1. Executive Summary & Purpose
This document presents an extensive, rigorous analysis and audit of the Haiku operating system kernel scheduler subsystem (`src/system/kernel/scheduler/`). It provides deep answers to critical architectural questions concerning lock contention, Layered Flat Bitmasks (LFB), EEVDF matrix updates, L3 cache grouping, work stealing, and opportunities for further lock refinement and performance optimization.

---

## 2. High-Load Interconnect Contention: Causes, Mechanics, and Fixes

### 2.1 EEVDF Matrix Updates
- **Mechanism**: The EEVDF scheduler quantizes time-slice deadlines into 32 secondary latency slots (SLI) across 16 primary priority rows (FLI), spanning a 512-lane flat bitmask (`fBitmap`). The quantization slot size (`gDeadlineBucketSize`) is dynamically scaled according to system interactivity (e.g., higher granularity when interactive workloads dominate, coarser under heavy batch compute).
- **The Bottleneck**:
  1. **Global Inter-Processor Interrupt (ICI) Broadcasts**: Whenever interactivity transitions occur, an update DPC triggers `smp_send_broadcast_ici()`. On many-core NUMA architectures (e.g., 64–512 CPUs across multiple sockets), issuing broadcast ICIs forces every core to stall, flush pipelines, and execute the interrupt handler.
  2. **Global Big Scheduler Lock (`InterruptsBigSchedulerLocker`)**: Changing `gDeadlineBucketSize`, its fixed-point reciprocal (`gDeadlineBucketReciprocal`), and recalculating thread quantum lengths across all cores acquires `gSchedulerUpdateLock`. This temporarily serializes scheduling across all CPUs.
  3. **RCU Synchronization Barriers**: In `InterruptsBigSchedulerLocking::~InterruptsBigSchedulerLocking()`, `scheduler_synchronize()` is executed. This forces CPUs to observe quiescent states, halting progress and causing severe cache-line bouncing on shared generation counters.
- **Solution / Mitigations**:
  - **Cooldown Dampening**: Maintain the 50ms cooldown timer (`sInteractivityState.lastAdjustment`) to coalesce rapid GUI state oscillations.
  - **Atomic Resolution Transitions (Double-Check Optimization)**: Perform atomic reads of `sInteractivityState.pendingDPCTarget` before acquiring the lock; if the bucket size already matches the target, bypass the lock and RCU synchronization entirely.
  - **Per-Core Decentralized Quantum Recalculation**: Instead of recalculating all threads globally while holding the big lock, perform lazy quantum re-quantization at thread replenishment time (`ThreadData::StartQuantum` / `Enqueue`).

### 2.2 CAS Loops in Hot Paths
- **Locations**:
  - `CoreEntry::_UpdateLoad`: Contention on `fCombinedLoad` (64-bit atomic packing load and epoch) and `fLoad`.
  - `CoreEntry::SetSystemVirtualTime`: CAS loop advancing SVT monotonically across concurrent logical CPUs.
  - `CPUEntry::ComputeLoad`: CAS loop on `fMeasureActiveTime`.
  - `UpdateIdleCoreLFB`: CAS loop updating `gIdleNodeSummary`.
- **The Bottleneck**: Under high core counts and heavy scheduling frequencies, multiple CPUs executing tight CAS loops (`AtomicTestAndSet64`) on the same cache line cause **cache-line ping-ponging** and interconnect saturation (MESI Invalidation Storms).
- **Solution / Mitigations**:
  - **Exponential Backoff with `cpu_pause()`**: Each failed CAS must issue `cpu_pause()` (e.g., the x86 `PAUSE` instruction, ARM `YIELD`), yielding the memory pipeline and preventing hyperthread starvation.
  - **Bounded Retry Limits with Best-Effort Degradation**: Cap CAS attempts (already applied: 32 to 64 retries). After hitting the cap, accept the latest coherent snapshot or use atomic release additions (`AddRelease`) rather than livelocking.
  - **Per-Socket / Per-Node Partitioning**: Keep load accumulation distributed across node hierarchies rather than aggregating into global atomic words.

---

## 3. Idle Discovery Scalability: Layered Flat Bitmask (LFB)

### 3.1 Architecture of LFB
Idle discovery in large SMP/NUMA topologies historically suffered from $O(N)$ tree traversal overhead. The Layered Flat Bitmask (LFB) optimizes this into a two-tiered atomic structure:
- **Tier 1 (`gIdleNodeSummary`)**: A single 64-bit atomic mask where bit $k$ is set if and only if Node $k$ contains at least one idle core.
- **Tier 2 (`gIdleCoresInNode[64]`)**: An array of 64-bit atomic masks, where bit $j$ in `gIdleCoresInNode[k]` represents core $j$ within Node $k$.
- **Mapping Matrix (`gNodeCoreMap[64][64]`)**: Direct $O(1)$ array dereference from `(nodeID, coreLocalIndex)` to `CoreEntry*`.

### 3.2 Complexity and Execution Flow
1. **Local Node Fast Path ($O(1)$)**: The scheduler inspects `gIdleCoresInNode[localNodeID]`. If non-zero, it invokes `scheduler_ctz()` to find the first idle core immediately, preserving NUMA locality and L3 cache warmth.
2. **Global Node Scan ($O(1)$)**: If the local node has no idle cores, the scheduler reads `gIdleNodeSummary`. With a single `scheduler_ctz()` it identifies a remote node with idle capacity and pulls a core from that node's mask.
3. Total time complexity is strictly $O(1)$, requiring at most 2 bit-scans and zero pointer-chasing loops.

### 3.3 Bottlenecks & Race Conditions
- **Node Summary Clearing Race**: When the last idle core in a node becomes active, it must clear its bit in `gIdleCoresInNode[nodeID]` and potentially clear its summary bit in `gIdleNodeSummary`. If another core concurrently goes idle, a naive write can lose the idle state.
- **Resolution**: `UpdateIdleCoreLFB` resolves this with a post-CAS re-verification loop: after clearing the node summary bit, it immediately re-reads `gIdleCoresInNode[nodeID]`; if any core went idle in the race window, it restores the summary bit using `AtomicOr64`.

---

## 4. Evaluation of L3 Grouping and Work Stealing

### 4.1 What L3 Grouping is Used For
- **Topology Structuring**: During boot (`init_topology`), CPUs sharing the same Last Level Cache (LLC/L3) are grouped into a `SchedulerNode` and partitioned into balanced clusters (`PackageEntry`).
- **Cache Locality Preservation**: Threads running on sibling cores in an L3 group share the massive LLC. When a thread wakes up or migrates, placement within the same L3 domain keeps cache misses to a minimum.
- **Multi-Tier Work Stealing**: When a CPU's run queue is depleted, it attempts lag-driven work stealing ("Laggiest Wins") in strict locality order:
  1. `_TryStealWorkL3`: Scans sibling cores in the same L3 package. Steals if `victim_lag >= gL3LagThreshold` (typically ~1ms).
  2. `_TryStealWorkNUMA`: Scans cores in the same NUMA node. Steals if `victim_lag >= gNUMANodeLagThreshold` (typically ~2ms).
  3. `_TryStealWorkGlobal`: Scans cores across other sockets/interconnects. Steals if `victim_lag >= gGlobalLagThreshold` (typically ~5ms).
  - Dynamic interconnect calibration (`scheduler_calibrate_interconnect`) measures round-trip memory access latency across domains at boot, scaling these thresholds automatically.

### 4.2 Is the Current L3 Grouping Ideal?
- **Strengths**:
  - Dynamically detects shared L3 IDs from architecture topology (`get_topology_id`).
  - Balances cluster sizes (target cluster size = 4 cores) to prevent oversized scheduling queues.
- **Limitations & Room for Improvement**:
  - **AMD Zen CCX / Multi-CCD Asymmetry**: On modern AMD processors (e.g., Ryzen / EPYC), multiple 8-core CCX complexes reside on a single socket, each with its own L3 cache. When BIOS reports uniform socket topology without distinctive L3 cache hierarchy, the fallback divides cores by 16 (`cpuList[l3Start] / 16`). On CCX systems with 6 or 8 cores per L3, this groups CPUs across CCX boundaries.
  - **Pseudo-Node Split Limit**: The hardcoded `kMaxCoresPerNode = 16` splits large L3 domains into pseudo-nodes. For modern monolithic server CPUs with 32–64 cores sharing a single L3, this creates artificial NUMA boundaries where cache lines are actually warm across all 32 cores.
  - **Ideal Enhancement**: Integrate direct ACPI PPTT (Processor Properties Topology Table) or CPUID leaf 0x4 / 0x1F cache hierarchy inspection into `arch_smp` so that CCX boundaries are always exact.

---

## 5. Global Spinlocks Analysis & Refinement Evaluation

### 5.1 Complete Inventory of Scheduler-Related Global Spinlocks

| Lock Name | Type | Location | Protected State |
| :--- | :--- | :--- | :--- |
| `gSchedulerUpdateLock` | `spinlock` | `scheduler.cpp:76` | Big scheduler update lock: mode switching, core online/offline, EEVDF resolution update DPC |
| `gSchedulerListenersLock` | `rw_spinlock` | `scheduler.cpp:465` | Registered scheduler listener chain (`gSchedulerListeners`) |
| `sThreadHashLock` | `rw_spinlock` | `thread.cpp:77` | Global thread hash lookup table |
| `gThreadCreationLock` | `spinlock` | `thread.cpp:86` | Global thread ID assignment and process thread insertion |
| `sUndertakerLock` | `spinlock` | `thread.cpp:148` | Thread death/burial queue |
| `sCachedKernelStacksLock` | `spinlock` | `thread.cpp:167` | Free kernel stack caching list |
| `gWaitObjectListenerLock` | `rw_spinlock` | `listeners.cpp:10` | Wait object listener registrations |

### 5.2 Should `gSchedulerUpdateLock` Be More Refined?
- **Current Role**: Acquired by `InterruptsBigSchedulerLocker` during:
  1. `scheduler_set_operation_mode` (rare admin event).
  2. `scheduler_set_cpu_enabled` (CPU hot-unplug / hot-plug).
  3. `update_quantum_lengths_dpc` (interactivity changes).
- **Refinement Assessment**:
  - **Mode Switches & Hotplug**: These are exceptionally rare runtime events. Retaining a global lock here is clean, safe, and avoids complex distributed state machine bugs.
  - **Quantum Length / EEVDF Updates**: This is the only path where `gSchedulerUpdateLock` can impact scheduling latency.
  - **Verdict**: `gSchedulerUpdateLock` has already been largely mitigated by ensuring `update_quantum_lengths_dpc` checks `sInteractivityState.pendingDPCTarget` before acquiring the lock. Further refinement into fine-grained per-node locks is unnecessary and would significantly increase code complexity without measurable throughput gains.

### 5.3 Should `gSchedulerListenersLock` Be More Refined?
- **Current Role**: Protects adding and removing trace/profiling listeners (`scheduler_add_listener`, `scheduler_remove_listener`).
- **Notification Path**: The hot path (`scheduler_enqueue_in_run_queue`, `scheduler_reschedule`, etc.) does **NOT** acquire `gSchedulerListenersLock`. Instead, it uses RCU-style lock-free atomic pointer traversal:
  ```cpp
  SchedulerListener* listener = atomic_pointer_get(&gSchedulerListeners);
  while (listener != NULL) {
      listener->...();
      listener = listener->fNext;
  }
  ```
- **Refinement Assessment**:
  - Registration/unregistration occurs only during debugging, tracing, or profiler startup/shutdown.
  - Because notification is 100% lock-free via RCU, there is **zero spinlock contention** on this lock during normal kernel execution.
  - **Verdict**: No further refinement needed; the lock is already optimal.

---

## 6. Bugs Discovered & Resolved in This Audit

1. **`src/system/kernel/scheduler/scheduler_cpu.cpp`**:
   - In `dump_idle_cores`, the outer loop `while (bits != 0)` for iterating the CPUSet word lacked a closing brace around the node inspection logic. This caused brace misalignment across `dump_idle_cores` and `init_debug_commands`.
   - **Fix**: Restored the missing closing brace and formatted the loop cleanly.

2. **`src/system/kernel/scheduler/low_latency.cpp`**:
   - In `choose_core`, inside the preferred core type scan (`if (preferMax || preferMin)`), the `while (bits != 0)` loop for iterating idle node mask bits was missing its terminating brace before `if (core == NULL)`.
   - **Fix**: Restored the missing closing brace and aligned indentation.

---

## 7. Summary & Recommendations

1. **Hot Path Contention**: The modernized BMQ-EEVDF implementation is robust. Memory barriers and atomics have been standardized to `LoadAcquire`/`StoreRelease`. CAS retry limits with `cpu_pause()` prevent interconnect livelocks.
2. **Idle Core Discovery**: LFB provides $O(1)$ discovery and solves the high-core-count scaling issues inherent in legacy tree walkers.
3. **L3 Grouping**: The current topology detection is well-suited for standard multicore systems. For future hardware revisions, integrating ACPI PPTT/CPUID leaf 0x1F parser will make AMD Zen CCX and multi-tile Intel grouping completely precise.
