# Haiku Scheduler Performance Bottlenecks & Optimization Plan (2025)

## 1. Core Scalability Features (Phase 3 Implemented)

*   **Scalable Idle Tracking:** Migrated to `CPUSet` to support systems with >64 logical processors.
*   **Scalable Runnable Tracking:** Replaced global atomic counter with decentralized per-CPU counts.
*   **Idle-Only Stealing:** Busy cores are never interrupted for rebalancing; throughput is maximized.
*   **Topology Awareness:** LLC and NUMA-aware work-stealing preserves data locality and cache warmth.

## 2. Identified Performance Bottlenecks (Phase 4 Pending)

### A. Static Work-Stealing Thresholds
Current lag thresholds (1ms/2ms/5ms) are hard-coded. On high-bandwidth or extremely low-latency interconnects, these might be too conservative.

### B. Heterogeneous Load Balancing overhead
Modern P/E core architectures require more frequent but low-overhead load updates to stay within the "efficiency sweet spot."

---

## 3. Phase 4 Task List

- [ ] **Task 1: Dynamic Interconnect-Aware Thresholds**
- [ ] **Task 2: Hardware-Guided EAS (Energy-Aware Scheduling)**
- [ ] **Task 3: Per-CPU DPC Queue Auditing**
