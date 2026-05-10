# Haiku Scheduler Audit Summary (2025)

## Overview
A comprehensive audit of the Haiku kernel scheduler was performed in February 2025. Technical debt in this subsystem is tracked using a custom `// Issue XX` numbering convention (1-100), rather than traditional `TODO` or `FIXME` tags.

## Key Findings and Fixes

### 1. Load Average Recalibration (Issue 16)
- **Problem:** Load average frequency was 5 seconds, leading to poor visibility of sub-second load spikes.
- **Fix:** Increased update frequency to 1 second in `scheduler_load.cpp`. Recalibrated EMA decay constants (`sCExp`) to {2014, 2041, 2046} for 1s ticks.

### 2. IRQ Drain Safety (Issue 15)
- **Problem:** The IRQ drain loop in `CPUEntry::Stop` used a hardcoded loop count.
- **Fix:** Refined logic to include a progress check. If `assign_io_interrupt_to_cpu` fails to move an IRQ from the head of the list, the loop aborts early with a diagnostic warning.

### 3. Node Limit Guards (Issue 74)
- **Problem:** On systems with >64 nodes, `gIdleNodeMask` (uint64) could be accessed out of bounds.
- **Fix:** Added explicit `fNodeID < 64` guards in `scheduler_cpu.h` for `PackageGoesIdle` and `PackageWakesUp`.

### 4. Lock Hierarchy Documentation (Issue 91)
- **Problem:** Potential for deadlock between `scheduler_lock` and core run-queue locks.
- **Fix:** Added comprehensive documentation in `scheduler.cpp` explaining the serialization requirements and the decoupling of the team-foreground path.

### 5. Atomic Safety and Alignment
- **Fix:** Applied `__attribute__((aligned(8)))` to all 64-bit atomic members (e.g., `fCombinedLoad`, `fStolenTime`) to ensure safety on 32-bit platforms.

## Conclusion
All 100 documented "Issues" have been either resolved or verified as correct behavior. The codebase is now free of active technical debt markers.
