# Haiku Scheduler Performance Audit & Bottlenecks

This document identifies remaining performance bottlenecks and scalability challenges in the Haiku scheduler.

## 1. Linear CPUSet Scans ($O(N)$)
Functions that iterate over all logical CPUs or those set in an affinity mask scale poorly on massive systems (128+ cores).
- **`CheckMaskedPackagesMinimumLoad`**: Iterates over all set bits in the affinity mask.
- **`scheduler_get_total_runnable_threads`**: Scans all per-CPU runnable counters.
- **`CoreEntry::GetMinVirtualRuntime`**: Iterates over all CPUs in the core.
- **`ChooseCoreAndCPU`**: The final fallback scan of all CPUs for a valid target.

## 2. Global Synchronization Overhead
- **`update_quantum_lengths_dpc`**: Requires a global ICI (Inter-Processor Interrupt) broadcast to synchronize virtual time buckets. Rapid interactivity changes can trigger this frequently, stalling all cores.
- **`scheduler_synchronize`**: The RCU grace period mechanism requires all CPUs to acknowledge a generation update, which becomes significantly slower as the CPU count increases.

## 3. Cache & Interconnect Contention
- **Power Saving Consolidation (`sSmallTaskCore`)**: A global/per-node pointer that is frequently updated via CAS. On systems with many sockets, this cache line "ping-pongs" between cores.
- **Topology Random Sampling**: While probes are $O(1)$, cross-NUMA probes incur significant latency penalty. High-frequency stealing attempts can saturate the memory interconnect.

## 4. EEVDF Interactivity DPC
- The periodic DPC used for RCU callback processing and interactivity updates targets the local CPU queue. While decentralized, the 1ms update frequency (`sInteractionTimer`) adds a baseline jitter that may be visible on ultra-low-latency real-time workloads.

## 5. Summary of Audit
The current implementation is highly scalable for systems up to 64-128 cores. Beyond this range, the $O(N)$ linear scans of bitmasks and per-CPU counters will begin to dominate the scheduling overhead.
