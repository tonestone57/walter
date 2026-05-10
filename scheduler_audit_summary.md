# Haiku Scheduler Audit & Refinement Summary (2025)

## 1. Sub-second Load Visibility (Issue 16)
- Increased load average update frequency from 5 seconds to 1 second.
- Recalibrated EMA decay constants (sCExp) to maintain accurate 1m, 5m, and 15m averages.

## 2. IRQ Draining Refinement (Issue 15)
- Documented and refined the IRQ draining logic in CPUEntry::Stop.
- Maintained a 1000-iteration safety bound while ensuring visibility of progress and potential truncation.

## 3. Concurrency & Documentation
- Formally audited the lock hierarchy in \`scheduler_on_team_foreground_changed\`.
- Added explicit documentation for criticalIssue fixes (1-100) throughout the scheduler files.
- Verified GCC 2.95 compatibility and architecture independence.
