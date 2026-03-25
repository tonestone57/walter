import re

# Update scheduler_audit_summary.md

with open("src/system/kernel/scheduler/scheduler_audit_summary.md", "r") as f:
    content = f.read()

# Modify point 8
pattern = r"### 8\. Power Saving Mode Parity.*?## Pending Recommendations"
replacement = """### 8. Power Saving Mode Parity
*   **Issue:** `power_saving.cpp` exhibited similar issues to those fixed in `low_latency.cpp` (unbounded linear fallback scans, global RNG contention) and additionally lacked Advanced NUMA Support and cache locality checks present in low-latency mode.
*   **Fix:** Brought `power_saving.cpp` up to full parity with `low_latency.cpp`:
    *   Limited fallback scans in `choose_core`, `rebalance`, and `rebalance_irqs` to 64 attempts (randomized start).
    *   Updated `search_local_node` and `search_global_random` to use per-CPU RNG and optimized collision detection.
    *   Added Cache Locality Support: Prefer scheduling threads on their `PreviousCore` or local sibling cores (L2/L3 domains).
    *   Added Advanced NUMA Support: Adjusted migration thresholds in `rebalance` based on `ThreadData::HomePackage()` to incentivize returning threads to their native memory domains.

## Pending Recommendations"""

content = re.sub(pattern, replacement, content, flags=re.DOTALL)

# Modify Conclusion
conclusion_pattern = r"Future work should focus on bringing `power_saving\.cpp` up to parity with `low_latency\.cpp`\."
content = re.sub(conclusion_pattern, "The power saving mode (`power_saving.cpp`) has been brought up to full architectural parity with the low latency mode.", content)

with open("src/system/kernel/scheduler/scheduler_audit_summary.md", "w") as f:
    f.write(content)

# Update scheduler_assessment.md

with open("src/system/kernel/scheduler/scheduler_assessment.md", "r") as f:
    content_assessment = f.read()

# Add a note about Power Saving Parity in Section 5
section_5_pattern = r"\*   \*\*Power Saving Mode:\*\*.*?CPU sleep states\."
section_5_replacement = """*   **Power Saving Mode:**
    *   **Strategy:** "Pack". Prefers filling up active cores before waking new ones.
    *   **Behavior:** Uses `choose_small_task_core` and `check_package_packing` to colocate threads.
    *   **Features:** Recently updated to achieve feature parity with Low Latency mode, including Advanced NUMA Support (Home Package migration thresholds) and Cache Locality (Previous Core / Sibling checks), applied as fallbacks when packing is not optimal.
    *   **Quantum:** Longer base quantum (5000us) to reduce context switches and allow deeper CPU sleep states."""

content_assessment = re.sub(section_5_pattern, section_5_replacement, content_assessment, flags=re.DOTALL)

with open("src/system/kernel/scheduler/scheduler_assessment.md", "w") as f:
    f.write(content_assessment)
