/*
 * Copyright 2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_LOCKING_H
#define KERNEL_SCHEDULER_LOCKING_H

#include <util/AutoLock.h>

#include "scheduler_cpu.h"

namespace Scheduler {


inline bool SchedulerLockHeld() {
	// Decentralized run-queues use interrupts-off as the baseline
	// requirement for scheduler operations.
	return !are_interrupts_enabled();
}

#define ASSERT_SCHED_LOCK() ASSERT(SchedulerLockHeld())

class SchedulerLockGuard {
public:
	SchedulerLockGuard() { Acquire(); }

	~SchedulerLockGuard() { Release(); }

private:
	void Acquire() {
		fStatus = disable_interrupts();
	}

	void Release() {
		restore_interrupts(fStatus);
	}

	cpu_status fStatus;
};

#ifdef DEBUG_SCHEDULER

enum SchedulerLockRank {
	LOCK_RANK_RUNQUEUE = 0,
	LOCK_RANK_THREAD = 1,
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

class CPURunQueueTryLocking {
public:
	inline bool Lock(CPUEntry* cpu) { return cpu->TryLockRunQueue(); }

	inline void Unlock(CPUEntry* cpu) { cpu->UnlockRunQueue(); }
};

typedef AutoLocker<CPUEntry, CPURunQueueTryLocking> CPURunQueueTryLocker;

class CoreRunQueueLocking {
public:
	inline bool Lock(CoreEntry* core) {
		return true;
	}

	inline void Unlock(CoreEntry* core) {}
};

typedef AutoLocker<CoreEntry, CoreRunQueueLocking> CoreRunQueueLocker;

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

		// RCU Synchronization: Synchronous wait replaced with eventual
		// consistency. Other CPUs will naturally reach a quiescent state
		// during their next reschedule. Asynchronous callbacks can be
		// used if specific cleanup is needed after the grace period.

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
