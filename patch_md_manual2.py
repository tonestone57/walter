with open("src/system/kernel/scheduler/scheduler_audit_summary.md", "r") as f:
    content = f.read()

import sys
if "Future work should focus" in content:
    print("Found text in audit")
else:
    print("Not found in audit")
