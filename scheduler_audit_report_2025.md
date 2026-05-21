# Haiku Kernel Scheduler Audit Report 2025

## Executive Summary
This audit explores the performance characteristics of the Haiku kernel scheduler on modern multi-core and NUMA hardware. We identify three primary bottlenecks that limit scalability and desktop responsiveness.

## Identified Bottlenecks

### 1. Global RCU Callback Contention
**Description:** RCU (Read-Copy Update) callbacks were managed via a single global list and spinlock.
**Impact:** On high-core count systems, concurrent thread destruction and mode switches lead to significant lock contention on 'sRCUCallbackLock'.
**Solution:** De-centralize RCU callback management by moving queues and locks to per-CPU 'CPUEntry' structures.

### 2. False Sharing on Interaction State
**Description:** Variables like 'sLastInteractionTime', 'sDPCPending', and 'sTimerArmed' were scattered in memory but often fell on the same or adjacent cache lines.
**Impact:** Cache-line 'bouncing' occurs when multiple CPUs update their interactivity metrics, leading to unnecessary interconnect traffic and stalls.
**Solution:** Group these variables into a 'struct InteractivityState' aligned to the CPU cache line size (64 bytes).

### 3. Flat Work-Stealing Sampling
**Description:** 'search_global_random' sampled packages uniformly across the entire system.
**Impact:** On large NUMA systems, this ignores memory and cache locality, leading to 'expensive' steals from remote nodes when local work might be available.
**Solution:** Refactor work-stealing to use a hierarchical strategy (Node -> Package) to prioritize locality while maintaining statistical coverage.

## Implementation Task List
- [x] De-centralize RCU callback queues to 'CPUEntry'.
- [x] Implement 'scheduler_process_rcu_callbacks' multi-CPU drain.
- [x] Group interactivity globals into 'InteractivityState' with 'CACHE_LINE_ALIGN'.
- [x] Refactor 'search_global_random' for hierarchical NUMA-aware sampling.
- [x] Align 'CPUEntry' and 'CoreEntry' to 64 bytes to prevent false sharing.
- [x] Consolidate 'TakeSnapshot' into 'scheduler_common.h'.
- [x] Fix static accessor naming consistency ('CPUEntry::Get').
