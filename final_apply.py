import sys
import re
import os

def fix_file(filepath, replacements):
    with open(filepath, 'r') as f:
        content = f.read()
    for old, new in replacements:
        content = content.replace(old, new)
    with open(filepath, 'w') as f:
        f.write(content)

# Issue 16: Load resolution
fix_file('src/system/kernel/scheduler/scheduler_load.cpp', [
    ('exp(-t / C) * kFScale, where t = 5s (update interval).', 'exp(-t / C) * kFScale, where t = 1s (update interval).'),
    ('1m: exp(-5 / 60)  * 2048 = 1884.21 ~= 1884', '1m: exp(-1 / 60)  * 2048 = 2014.15 ~= 2014'),
    ('5m: exp(-5 / 300) * 2048 = 2014.15 ~= 2014', '5m: exp(-1 / 300) * 2048 = 2041.18 ~= 2041'),
    ('15m: exp(-5 / 900) * 2048 = 2036.64 ~= 2037', '15m: exp(-1 / 900) * 2048 = 2045.72 ~= 2046'),
    ('const static uint64 sCExp[3] __attribute__((aligned(8))) = { 1884, 2014, 2037 };', 'const static uint64 sCExp[3] __attribute__((aligned(8))) = { 2014, 2041, 2046 };'),
    ('// Issue 16: gTotalRunnableThreads', '// Issue 16 fix: gTotalRunnableThreads'),
    ('every 5 seconds.', 'every 1 second.'),
    ('within the 5-second', 'within the 1-second'),
    ('5-second tick),', '1-second tick),'),
    ('register_kernel_daemon(_LoadavgUpdate, NULL, 5000000);', 'register_kernel_daemon(_LoadavgUpdate, NULL, 1000000);'),
    ('// run the daemon every five seconds (5,000,000 µs)', '// run the daemon every second (1,000,000 µs)')
])

# Issue 15: IRQ draining
with open('src/system/kernel/scheduler/scheduler_cpu.cpp', 'r') as f:
    cpu_content = f.read()

old_irq = """	// get rid of irqs
	SpinLocker locker(entry->irqs_lock);
	for (int32 i = 0; i < 1000; i++) {
		irq_assignment* irq
			= (irq_assignment*)list_get_first_item(&entry->irqs);
		if (irq == NULL)
			break;"""

new_irq = """	// get rid of irqs
	int32 maxIterations = 0;
	{
		SpinLocker countLocker(entry->irqs_lock);
		irq_assignment* irq = (irq_assignment*)list_get_first_item(&entry->irqs);
		while (irq != NULL) {
			maxIterations++;
			irq = (irq_assignment*)list_get_next_item(&entry->irqs, irq);
		}
	}
	maxIterations += 10;

	SpinLocker locker(entry->irqs_lock);
	for (int32 i = 0; i < maxIterations; i++) {
		irq_assignment* irq
			= (irq_assignment*)list_get_first_item(&entry->irqs);
		if (irq == NULL)
			break;"""

cpu_content = cpu_content.replace(old_irq, new_irq)
cpu_content = cpu_content.replace('burning all 1000 iterations.', 'burning all iterations.')
cpu_content = cpu_content.replace('dprintf("CPUEntry::Stop: safety limit reached while removing "',
                                  'dprintf("CPUEntry::Stop: safety limit reached (%" B_PRId32 " iterations) while removing ",\n\t\t\tmaxIterations')
with open('src/system/kernel/scheduler/scheduler_cpu.cpp', 'w') as f:
    f.write(cpu_content)

# Issue 74: Header braces
with open('src/system/kernel/scheduler/scheduler_cpu.h', 'r') as f:
    h_content = f.read()

h_content = h_content.replace('if (fNodeID < 64)\n\t\t\tatomic_or64((int64*)&gIdleNodeMask, 1ULL << fNodeID);',
                              'if (fNodeID < 64) { // Issue 74 fix: node limit\n\t\t\tatomic_or64((int64*)&gIdleNodeMask, 1ULL << fNodeID);\n\t\t}')
h_content = h_content.replace('if (fNodeID < 64)\n\t\t\tatomic_and64((int64*)&gIdleNodeMask, ~(1ULL << fNodeID));',
                              'if (fNodeID < 64) { // Issue 74 fix: node limit\n\t\t\tatomic_and64((int64*)&gIdleNodeMask, ~(1ULL << fNodeID));\n\t\t}')
with open('src/system/kernel/scheduler/scheduler_cpu.h', 'w') as f:
    f.write(h_content)
