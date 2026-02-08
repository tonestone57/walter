# Scheduler Audit Findings

## 1. Compilation Error: `assign_io_interrupt_to_cpu` Return Type Mismatch

**File:** `src/system/kernel/scheduler/scheduler_cpu.cpp`
**Function:** `CPUEntry::Stop()`

**Description:**
The function `assign_io_interrupt_to_cpu` is declared in `headers/private/kernel/interrupts.h` and implemented in `src/system/kernel/interrupts.cpp` as returning `void`. However, `CPUEntry::Stop()` attempts to assign its return value to a `status_t` variable and checks it against `B_OK`. This results in a compilation error.

**Code:**
```cpp
status_t status = assign_io_interrupt_to_cpu(irqVector, -1);
if (status != B_OK) {
    dprintf("CPUEntry::Stop: failed to unassign interrupt %" B_PRId32
        ": %s. Trying current CPU.\n", irqVector, strerror(status));
    status = assign_io_interrupt_to_cpu(irqVector, smp_get_current_cpu());
}
```

**Recommendation:**
Remove the return value check. Since `assign_io_interrupt_to_cpu` handles reassignment internally (potentially falling back to `assign_cpu()`), the caller cannot easily determine success/failure. If failure detection is needed, `assign_io_interrupt_to_cpu` should be modified to return `status_t`.

## 2. Fragile VTable Wiping in `scheduling_analysis.cpp`

**File:** `src/system/kernel/scheduler/scheduling_analysis.cpp`
**Function:** `SchedulingAnalysisManager::FinishAnalysis()`

**Description:**
The function attempts to clear the `next` pointer of `HashObject` derived classes (`Thread`, `WaitObject`) in the user buffer to prevent leaking kernel heap addresses (or just cleanup). It does so by writing `NULL` to the first word of the object: `*(void**)object = NULL;`.
However, `HashObject` has virtual functions, so the first word of the object is the vtable pointer (vptr), not the `next` member (which is likely at offset `sizeof(void*)`).
This effectively wipes the vptr, which is good for security (prevents KASLR leak of kernel vtable address), but relies on compiler-specific ABI (vptr at offset 0). If the ABI changes, this code might corrupt other data or fail to wipe the vptr.

**Code:**
```cpp
HashObject* next = object->next;
*(void**)object = NULL;
object = next;
```

**Recommendation:**
Explicitly handle the vptr wiping if intended, or use `object->next = NULL` if the intention was to clear the next pointer. Given the context (exporting to user space), clearing the vptr is likely the hidden goal, but it should be documented or done more robustly.

## 3. Potential Infinite Loop / Logic Error in `power_saving.cpp`

**File:** `src/system/kernel/scheduler/power_saving.cpp`
**Function:** `pack_irqs` (called by `rebalance_irqs`)

**Description:**
The function iterates over IRQs assigned to the current CPU and attempts to move them to a "small task core". It breaks the loop if the IRQ at the head of the list remains the same after an attempt. However, the logic relies on side effects of `assign_io_interrupt_to_cpu` (which is void) to determine progress.
Also, `pack_irqs` is called directly from `rebalance_irqs` in the idle path, which runs in the scheduler context (interrupts disabled). Calling `assign_io_interrupt_to_cpu` (which acquires spinlocks and performs arch-specific operations) from `reschedule()` is risky and can lead to high latency or deadlocks. In contrast, `low_latency.cpp` and the non-idle path of `power_saving.cpp` use a DPC (`IRQRebalanceDPC`) to perform IRQ reassignment outside the critical scheduler path.

**Recommendation:**
Move `pack_irqs` logic to a DPC or ensure `assign_io_interrupt_to_cpu` is safe to call from scheduler context.
