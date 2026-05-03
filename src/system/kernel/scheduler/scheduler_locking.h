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


inline bool
SchedulerLockHeld()
{
#ifdef DEBUG_SCHEDULER
	Thread* thread = thread_get_current_thread();
	return thread != NULL && thread->scheduler_lock_depth > 0;
#else
	return true; // assume correct in release
#endif
}


#define ASSERT_SCHED_LOCK() ASSERT(SchedulerLockHeld())


class SchedulerLockGuard {
public:
	SchedulerLockGuard()
	{
		Acquire();
	}

	~SchedulerLockGuard()
	{
		Release();
	}

private:
	void Acquire()
	{
		fStatus = disable_interrupts();

#ifdef DEBUG_SCHEDULER
		// Use get_cpu_struct()->running_thread for safer access during
		// context switches and early boot.
		Thread* thread = get_cpu_struct()->running_thread;
		if (thread != NULL)
			thread->scheduler_lock_depth++;
#endif

		AcquireSchedulerSpinlock();
	}

	void Release()
	{
		ReleaseSchedulerSpinlock();

#ifdef DEBUG_SCHEDULER
		Thread* thread = get_cpu_struct()->running_thread;
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
	LOCK_RANK_RUNQUEUE  = 1,
	LOCK_RANK_THREAD    = 2,
};


inline void
AssertLockOrder(int rank)
{
	Thread* thread = thread_get_current_thread();
	if (thread != NULL) {
		ASSERT(rank >= thread->current_lock_rank);
		thread->current_lock_rank = rank;
	}
}


inline void
ReleaseLockOrder(int rank)
{
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
	InterruptGuard()
	{
		fWasEnabled = are_interrupts_enabled();
		if (fWasEnabled)
			disable_interrupts();
	}

	~InterruptGuard()
	{
		if (fWasEnabled)
			enable_interrupts();
	}

private:
	bool fWasEnabled;
};


#define SCHEDULER_CRITICAL_SECTION() \
	SchedulerLockGuard _schedLockGuard;


#ifdef DEBUG_SCHEDULER

inline void
AssertInterruptsDisabled()
{
	ASSERT(!are_interrupts_enabled());
}

#define ASSERT_IRQ_DISABLED() AssertInterruptsDisabled()

#else

#define ASSERT_IRQ_DISABLED() ((void)0)

#endif


class CPURunQueueLocking {
public:
	inline bool Lock(CPUEntry* cpu)
	{
		cpu->LockRunQueue();
		return true;
	}

	inline void Unlock(CPUEntry* cpu)
	{
		cpu->UnlockRunQueue();
	}
};

typedef AutoLocker<CPUEntry, CPURunQueueLocking> CPURunQueueLocker;


class CoreRunQueueLocking {
public:
	inline bool Lock(CoreEntry* core)
	{
		core->LockRunQueue();
		return true;
	}

	inline void Unlock(CoreEntry* core)
	{
		core->UnlockRunQueue();
	}
};

class CoreRunQueueTryLocking {
public:
	inline bool Lock(CoreEntry* core)
	{
		return core->TryLockRunQueue();
	}

	inline void Unlock(CoreEntry* core)
	{
		core->UnlockRunQueue();
	}
};

typedef AutoLocker<CoreEntry, CoreRunQueueTryLocking> CoreRunQueueTryLocker;

typedef AutoLocker<CoreEntry, CoreRunQueueLocking> CoreRunQueueLocker;

class CoreCPUHeapLocking {
public:
	inline bool Lock(CoreEntry* core)
	{
		core->LockCPUHeap();
		return true;
	}

	inline void Unlock(CoreEntry* core)
	{
		core->UnlockCPUHeap();
	}
};

typedef AutoLocker<CoreEntry, CoreCPUHeapLocking> CoreCPUHeapLocker;


class CoreCPULocking {
public:
	inline bool Lock(CoreEntry* core)
	{
		core->LockCPU();
		return true;
	}

	inline void Unlock(CoreEntry* core)
	{
		core->UnlockCPU();
	}
};

typedef AutoLocker<CoreEntry, CoreCPULocking> CoreCPULocker;

class SchedulerModeLocking {
public:
	bool Lock(int* /* lockable */)
	{
		CPUEntry::GetCPU(smp_get_current_cpu())->EnterScheduler();
		return true;
	}

	void Unlock(int* /* lockable */)
	{
		CPUEntry::GetCPU(smp_get_current_cpu())->ExitScheduler();
	}
};

class SchedulerModeLocker :
	public AutoLocker<int, SchedulerModeLocking> {
public:
	SchedulerModeLocker(bool alreadyLocked = false, bool lockIfNotLocked = true)
		:
		AutoLocker<int, SchedulerModeLocking>(&fDummy, alreadyLocked,
			lockIfNotLocked)
	{
	}

private:
	int		fDummy;
};

class InterruptsSchedulerModeLocking {
public:
	bool Lock(int* lockable)
	{
		*lockable = disable_interrupts();
		CPUEntry::GetCPU(smp_get_current_cpu())->EnterScheduler();
		return true;
	}

	void Unlock(int* lockable)
	{
		CPUEntry::GetCPU(smp_get_current_cpu())->ExitScheduler();
		restore_interrupts(*lockable);
	}
};

class InterruptsSchedulerModeLocker :
	public AutoLocker<int, InterruptsSchedulerModeLocking> {
public:
	InterruptsSchedulerModeLocker(bool alreadyLocked = false,
		bool lockIfNotLocked = true)
		:
		AutoLocker<int, InterruptsSchedulerModeLocking>(&fState, alreadyLocked,
			lockIfNotLocked)
	{
	}

private:
	int		fState;
};

class InterruptsBigSchedulerLocking {
public:
	bool Lock(int* lockable)
	{
		*lockable = disable_interrupts();
		for (int32 i = 0; i < smp_get_num_cpus(); i++)
			CPUEntry::GetCPU(i)->LockScheduler();
		return true;
	}

	void Unlock(int* lockable)
	{
		for (int32 i = 0; i < smp_get_num_cpus(); i++)
			CPUEntry::GetCPU(i)->UnlockScheduler();
		restore_interrupts(*lockable);
	}
};

class InterruptsBigSchedulerLocker :
	public AutoLocker<int, InterruptsBigSchedulerLocking> {
public:
	InterruptsBigSchedulerLocker()
		:
		AutoLocker<int, InterruptsBigSchedulerLocking>(&fState, false, true)
	{
	}

private:
	int		fState;
};


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_LOCKING_H

