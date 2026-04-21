# Scheduler Audit Findings

## 1. Compilation Error: `assign_io_interrupt_to_cpu` Return Type Mismatch [FIXED]

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

**Resolution:**
The return value assignment and check were removed. `assign_io_interrupt_to_cpu` is now called as a void function.

## 2. Fragile VTable Wiping in `scheduling_analysis.cpp` [FIXED]

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

**Resolution:**
Replaced the pointer overwrite with `memset(object, 0, sizeof(HashObject))` to safely and portably clear the base class portion of the object (including vtable pointer and `next` pointer).

## 3. Potential Infinite Loop / Logic Error in `power_saving.cpp` [FIXED]

**File:** `src/system/kernel/scheduler/power_saving.cpp`
**Function:** `pack_irqs` (called by `rebalance_irqs`)

**Description:**
The function iterates over IRQs assigned to the current CPU and attempts to move them to a "small task core". It breaks the loop if the IRQ at the head of the list remains the same after an attempt. However, the logic relies on side effects of `assign_io_interrupt_to_cpu` (which is void) to determine progress.
Also, `pack_irqs` is called directly from `rebalance_irqs` in the idle path, which runs in the scheduler context (interrupts disabled). Calling `assign_io_interrupt_to_cpu` (which acquires spinlocks and performs arch-specific operations) from `reschedule()` is risky and can lead to high latency or deadlocks. In contrast, `low_latency.cpp` and the non-idle path of `power_saving.cpp` use a DPC (`IRQRebalanceDPC`) to perform IRQ reassignment outside the critical scheduler path.

**Resolution:**
The `pack_irqs` function was removed. `rebalance_irqs` was updated to handle the "pack to small task core" case by scheduling an `IRQRebalanceDPC` to move a single IRQ, consistent with the standard rebalancing logic and avoiding execution in the scheduler context.

---

## Audit Log

### Deep Dive: Work Stealing & Locking
**Status:** Verified Safe.
- **`try_acquire_spinlock` in `_TryStealWork`:** Failure to acquire the lock correctly leads to skipping the victim core, which is the desired behavior to avoid contention or deadlock. No systematic starvation risk detected.
- **Locking Hierarchy:** The interaction between `scheduler_set_operation_mode` (locks all CPUs) and `scheduler_set_cpu_enabled` (locks single CPU) was analyzed. No circular dependencies were found as granular locks are leaf locks in this context.

### Deep Dive: Scheduler Listeners
**Status:** Verified Safe.
- **Recursive Locking:** `SystemProfiler` can trigger recursive calls to `NotifySchedulerListeners` (via `thread_unblock_waker`). The kernel's `rw_spinlock` implementation (reader-biased) allows recursive read acquisition, preventing deadlock in this scenario.
- **Lock Inversion:** No inversion found between `fLock` (in `SystemProfiler`) and `gSchedulerListenersLock`.

### Deep Dive: Thread Migration
**Status:** Verified Safe.
- **Race Conditions:** The interaction between `ChooseCoreAndCPU` and `Enqueue` handles concurrent CPU disabling correctly. The `Enqueue` method re-verifies `fCore->CPUCount()` under the runqueue lock and forces a retry if the core has gone offline.

## 4. Stolen Thread Loss during Hot-Unplug [FIXED]
**File:** `src/system/kernel/scheduler/scheduler_cpu.cpp`
**Function:** `CPUEntry::ChooseNextThread()`
**Description:** Stolen threads could be lost if the second enqueue attempt failed due to a concurrent hot-unplug.
**Resolution:** Added log warning and forced fallback to current CPU's core to prevent permanent thread leaks.

## 5. NULL Dereference in Enqueue [FIXED]
**File:** `src/system/kernel/scheduler/scheduler_thread.h`
**Function:** `ThreadData::Enqueue()`
**Description:** `fCore` could be NULL during a hot-unplug race, causing a crash when constructing RAII lockers.
**Resolution:** Added a NULL guard for `fCore` before locker construction and a second check under the lock.

## 6. Dying Thread Re-enqueue [FIXED]
**File:** `src/system/kernel/scheduler/scheduler.cpp`
**Function:** `reschedule()`
**Description:** A thread in `THREAD_STATE_FREE_ON_RESCHED` could theoretically be re-enqueued if migration logic interfered.
**Resolution:** Explicitly cleared `enqueueOldThread` for dying threads.
