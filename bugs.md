# Haiku Scheduler Audit Report (2025)

## 1. Concurrency & Race Conditions

### 1.1 Interaction State DPC Loss
In `scheduler_update_interaction_state` (`scheduler.cpp`), if a DPC is already in flight (`sDPCPending == 1`), subsequent requests to change the resolution (e.g., downscaling from 1000 to 5000) may be lost.
*   **Impact:** Transiently incorrect scheduling resolution (quantum lengths).
*   **Status:** **Resolved**. `update_quantum_lengths_dpc` now uses an atomic loop to re-check for pending requests after synchronization. Redundant global synchronization was removed to improve efficiency.

### 1.2 fReady Race in Continues()
In `ThreadData::Continues` (`scheduler_thread.h`), the check for `fReady` is performed without holding the core run-queue lock. A concurrent CPU calling `GoesAway` on the same thread (e.g., during a rapid reschedule) can clear `fReady` between the time the thread is selected and `Continues` is called.
*   **Impact:** Spurious debug warnings; potential for skipped load updates.
*   **Mitigation:** Already mitigated in existing source via `dprintf` instead of a hard `ASSERT`.

### 1.3 gTotalRunnableThreads Race
The global counter `gTotalRunnableThreads` is updated via `AddAcquireRelease`, but under extreme contention, it can transiently enter a negative state.
*   **Impact:** Inaccurate load averaging for the global system.
*   **Mitigation:** Already mitigated in existing source via a manual correction block in the `enqueue` retry loop in `scheduler.cpp`.

### 1.4 fCore Snapshot in GoesAway
`ThreadData::GoesAway` previously performed multiple dereferences of `fCore`. Since `fCore` can be set to NULL by a concurrent `MigrateTo` (called from another CPU), this was a race leading to potential NULL dereferences.
*   **Impact:** Kernel Panic.
*   **Mitigation:** Already mitigated in existing source via a single atomic snapshot of `fCore`.

## 2. Potential Livelocks & Performance

### 2.1 Core Load Update Contention
`CoreEntry::_UpdateLoad` (`scheduler_cpu.cpp`) uses a nested CAS loop on `fCombinedLoad` and `fLoad`. Under pathological contention where many CPUs are constantly updating the core load, a single CPU could theoretically spin indefinitely.
*   **Impact:** High scheduling latency/jitter on specific CPUs.
*   **Mitigation:** Already mitigated in existing source via retry bounds (`kMaxCombinedRetries`, `kMaxFLoadRetries`).

### 2.2 Work-Stealing Collision
`search_local_node` and `search_global_random` in `scheduler_topology.h` use random sampling. While collision detection is implemented via bitmasks, for systems with more than 64 packages (local) or 4096 packages (global), the deduplication becomes less effective or is skipped.
*   **Impact:** Wasted budget/cycles probing the same victim multiple times.
*   **Status:** **Resolved**. `search_local_node` refactored with an expanded 512-bit stack-allocated bitmask and a robust `while` loop that counts unique successful probes, ensuring sampling depth is always reached.

## 3. Logic & Boundary Errors

### 3.1 IRQ Drain Truncation
`CPUEntry::Stop` (`scheduler_cpu.cpp`) limits the IRQ draining loop to 1000 iterations. If a CPU has more than 1000 IRQs assigned (rare but possible on extreme hardware or with bugged drivers), the remaining IRQs are not reassigned.
*   **Impact:** Devices associated with those IRQs may stop responding after the CPU is disabled.
*   **Status:** **Improved**. `kMaxIterations` increased to 4096 to ensure reliable reassignment on high-end hardware.

### 3.2 fRescheduleCount Wrap-around
In `UpdatePriorityBoostScalable`, the modular ownership calculation `(boostEpoch % coreCPUCount)` relied on a post-increment of `fRescheduleCount`. At the `UINT32_MAX -> 0` boundary, all CPUs on a core could simultaneously satisfy the `(0 % 10 == 0)` check.
*   **Impact:** Correlated lock contention spikes across all CPUs every 4.2 billion reschedules.
*   **Mitigation:** Already mitigated in existing source via clamping and non-zero fallback logic.

### 3.3 EEVDF Eligibility Deadlock (Potential)
In `RunQueue.h`, `CheckEligibility` attempts to migrate threads from the ineligible heap to the eligible heap. If the eligible heap is full (`fEligibleCount >= kMaxThreadsPerCore`), it breaks the loop.
*   **Impact:** Threads remain ineligible longer than mathematically required.
*   **Status:** **Improved**. `kMaxThreadsPerCore` has been increased to 2048 to provide a larger safety margin.

### 3.4 Transient fCore NULL in ComputeQuantum
In `ThreadData::ComputeQuantum` (`scheduler_thread.cpp`), `fCore` is snapshotted via `atomic_pointer_get`. However, if the snapshot returns `NULL` (due to a race with `UnassignCore`), the function returns a minimal quantum.
*   **Impact:** Suboptimal quantum selection during rapid thread migration or hot-plug.
*   **Status:** **Improved**. The function now attempts to use `PreviousCore()` as a fallback scaling hint.

## 4. Portability & Coding Standards

### 4.1 64-bit Alignment Requirements
Atomic operations on 64-bit variables (`fVirtualRuntime`, `gIdleMask`, etc.) require 8-byte alignment on many 32-bit platforms to ensure atomicity.
*   **Observation:** The subsystem correctly uses `__attribute__((aligned(8)))` for these variables, but any new 64-bit state must strictly follow this pattern.

### 4.2 Pointer Type Safety in Atomics
Standard Haiku `atomic_*` functions often expect `int32*` or `int64*`. The scheduler's use of template wrappers with `reinterpret_cast<int32 volatile*>` is necessary for GCC 13 but bypasses some of the compiler's natural type checking.
*   **Observation:** The alignment attributes and `static_assert` guards in `scheduler_common.h` mitigate the risk of incorrect sizes.

### 4.3 Typographical Consistency
Inconsistent spelling of technical terms (e.g., "behaviour" vs "behavior") and case-sensitivity in member naming (e.g., `lastReschedule`) exists.
*   **Impact:** Minor inconsistency in log files and source code readability.
*   **Status:** **Resolved**. Member renamed to `fLastReschedule` and all subsystem spellings standardized to American English (behavior, optimization, initialization, color, honor, etc.).

### 4.4 Shift Domain Safety
Bitwise calculations for core ownership modular logic in `UpdatePriorityBoostScalable` could trigger undefined behavior if shift domains exceeded the bit width of `native_cpu_mask_t`.
*   **Status:** **Resolved**. Explicit clamps and branch guards added to ensure valid shift domains on all supported architectures.
