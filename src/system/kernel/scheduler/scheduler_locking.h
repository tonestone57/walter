/*
 * Copyright 2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_LOCKING_H
#define KERNEL_SCHEDULER_LOCKING_H

#include <util/AutoLock.h>

#include "scheduler_cpu.h"

namespace Scheduler {

extern "C" void AcquireSchedulerSpinlock();
extern "C" void ReleaseSchedulerSpinlock();

inline bool SchedulerLockHeld() {
	Thread* thread = get_cpu_struct()->running_thread;
	return thread == NULL || thread->scheduler_lock_depth > 0;
}

#define ASSERT_SCHED_LOCK() ASSERT(SchedulerLockHeld())

class SchedulerLockGuard {
public:
	SchedulerLockGuard() { Acquire(); }

	~SchedulerLockGuard() { Release(); }

private:
	void Acquire() {
		fStatus = disable_interrupts();

		// Decentralized protection: acquire the current thread's scheduler_lock.
		// This provides execution-state protection for code that previously
		// relied on the global gSchedulerLock.
		Thread* thread = get_cpu_struct()->running_thread;
		if (thread != NULL)
			acquire_spinlock(&thread->scheduler_lock);

#ifdef DEBUG_SCHEDULER
		if (thread != NULL)
			thread->scheduler_lock_depth++;
#endif
	}

	void Release() {
		Thread* thread = get_cpu_struct()->running_thread;
		if (thread != NULL)
			release_spinlock(&thread->scheduler_lock);

#ifdef DEBUG_SCHEDULER
		if (thread != NULL) {
			thread->scheduler_lock_depth--;
			ASSERT(thread->scheduler_lock_depth >= 0);
		}
#endif

		restore_interrupts(fStatus);
	}

	cpu_status fStatus;
};

#ifdef DEBUG_SCHEDULER

enum SchedulerLockRank {
	LOCK_RANK_SCHEDULER = 0,
	LOCK_RANK_RUNQUEUE = 1,
	LOCK_RANK_THREAD = 2,
};

inline void AssertLockOrder(int rank) {
	Thread* thread = thread_get_current_thread();
	if (thread != NULL) {
		ASSERT(rank >= thread->current_lock_rank);
		thread->current_lock_rank = rank;
	}
}

inline void ReleaseLockOrder(int rank) {
	Thread* thread = thread_get_current_thread();
	if (thread != NULL) {
		ASSERT(thread->current_lock_rank == rank);
		thread->current_lock_rank--;
	}
}

#else

inline void AssertLockOrder(int) {}
inline void ReleaseLockOrder(int) {}

#endif

class InterruptGuard {
public:
	InterruptGuard() : fStatus(disable_interrupts()) {}

	~InterruptGuard() { restore_interrupts(fStatus); }

private:
	cpu_status fStatus;
};

#define SCHEDULER_CRITICAL_SECTION() SchedulerLockGuard _schedLockGuard;

#ifdef DEBUG_SCHEDULER

inline void AssertInterruptsDisabled() { ASSERT(!are_interrupts_enabled()); }

#define ASSERT_IRQ_DISABLED() AssertInterruptsDisabled()

#else

#define ASSERT_IRQ_DISABLED() ((void)0)

#endif

class CPURunQueueLocking {
public:
	inline bool Lock(CPUEntry* cpu) {
		cpu->LockRunQueue();
		return true;
	}

	inline void Unlock(CPUEntry* cpu) { cpu->UnlockRunQueue(); }
};

typedef AutoLocker<CPUEntry, CPURunQueueLocking> CPURunQueueLocker;


class CoreCPUHeapLocking {
public:
	inline bool Lock(CoreEntry* core) {
		core->LockCPUHeap();
		return true;
	}

	inline void Unlock(CoreEntry* core) { core->UnlockCPUHeap(); }
};

typedef AutoLocker<CoreEntry, CoreCPUHeapLocking> CoreCPUHeapLocker;

class CoreCPULocking {
public:
	inline bool Lock(CoreEntry* core) {
		core->LockCPU();
		return true;
	}

	inline void Unlock(CoreEntry* core) { core->UnlockCPU(); }
};

typedef AutoLocker<CoreEntry, CoreCPULocking> CoreCPULocker;

class SchedulerModeLocking {
public:
	bool Lock(int* /* lockable */) {
		// RCU Read Section: Wait-free on Haiku.
		// reschedule() ensures the CPU's fRCULastGeneration is updated.
		return true;
	}

	void Unlock(int* /* lockable */) {
	}
};

class SchedulerModeLocker : public AutoLocker<int, SchedulerModeLocking> {
public:
	SchedulerModeLocker(bool alreadyLocked = false, bool lockIfNotLocked = true)
		: AutoLocker<int, SchedulerModeLocking>(&fDummy, alreadyLocked,
												lockIfNotLocked) {}

private:
	int fDummy;
};

class InterruptsSchedulerModeLocking {
public:
	bool Lock(int* lockable) {
		*lockable = disable_interrupts();
		return true;
	}

	void Unlock(int* lockable) {
		restore_interrupts(*lockable);
	}
};

class InterruptsSchedulerModeLocker
	: public AutoLocker<int, InterruptsSchedulerModeLocking> {
public:
	InterruptsSchedulerModeLocker(bool alreadyLocked = false,
								  bool lockIfNotLocked = true)
		: AutoLocker<int, InterruptsSchedulerModeLocking>(
			  &fState, alreadyLocked, lockIfNotLocked) {}

private:
	int fState;
};

class InterruptsBigSchedulerLocking {
public:
	bool Lock(int* lockable) {
		*lockable = disable_interrupts();
		acquire_spinlock(&gSchedulerUpdateLock);
		return true;
	}

	void Unlock(int* lockable) {
		release_spinlock(&gSchedulerUpdateLock);

		// RCU Synchronization: Wait for all CPUs to reach a quiescent state
		// (reschedule) before allowing the caller to proceed. This ensures
		// that no CPU is still using the old scheduler mode data.
		scheduler_synchronize();

		restore_interrupts(*lockable);
	}
};

class InterruptsBigSchedulerLocker
	: public AutoLocker<int, InterruptsBigSchedulerLocking> {
public:
	InterruptsBigSchedulerLocker()
		: AutoLocker<int, InterruptsBigSchedulerLocking>(&fState, false, true) {
	}

private:
	int fState;
};

}  // namespace Scheduler

#endif	// KERNEL_SCHEDULER_LOCKING_H
