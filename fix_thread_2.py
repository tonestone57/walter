import re
path = 'src/system/kernel/scheduler/scheduler_thread.h'
with open(path, 'r') as f:
    content = f.read()

# Replace the first occurrence
content = re.sub(
    r'atomic_pointer_get<CoreEntry>\(\s*const_cast<CoreEntry\* volatile\*>\(&fCore\)\)',
    r'atomic_pointer_get<CoreEntry>(&fCore)',
    content,
    flags=re.MULTILINE
)

with open(path, 'w') as f:
    f.write(content)
