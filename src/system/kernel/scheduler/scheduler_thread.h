/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_THREAD_H
#define KERNEL_SCHEDULER_THREAD_H


#include <thread.h>
#include <util/AutoLock.h>

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_locking.h"
#include "scheduler_profiler.h"


namespace Scheduler {


struct CACHE_LINE_ALIGN ThreadData : public DoublyLinkedListLinkImpl<ThreadData>,
	RunQueueLinkImpl<ThreadData> {
private:
	inline	void		_InitBase();

	SCHEDULER_INLINE	CoreEntry*	_ChooseCore() const;
	SCHEDULER_INLINE	CPUEntry*	_ChooseCPU(CoreEntry* core,
							bool& rescheduleNeeded) const;

public:
						ThreadData(Thread* thread);

			void		Init();
			void		Init(CoreEntry* core);

			void		Dump() const;

	SCHEDULER_INLINE	int32		GetPriority() const	{ return fThread->priority; }
	SCHEDULER_INLINE	Thread*		GetThread() const	{ return fThread; }
	// gCPUEnabled is updated one word at a time by SetBitAtomic/
	// ClearBitAtomic; there is no compound-And atomicity guarantee.  Read
	// each word with atomic_get to minimise torn-read exposure across the
	// word boundary (most relevant on systems with >32 CPUs).
	SCHEDULER_INLINE	CPUSet		GetCPUMask() const
	{
		CPUSet enabled;
		const int32 kWords = (SMP_MAX_CPUS + 31) / 32;
		for (int32 i = 0; i < kWords; i++) {
			uint32 w = (uint32)atomic_get(
				const_cast<int32*>((const int32*)gCPUEnabled.Bits() + i));
			enabled.SetWord(i, w);
		}
		return fThread->cpumask.And(enabled);
	}

	SCHEDULER_INLINE	bool		IsRealTime() const;
	SCHEDULER_INLINE	bool		IsIdle() const;

	SCHEDULER_INLINE	bool		HasCacheExpired() const;
	SCHEDULER_INLINE	int32		HomePackage() const { return fHomePackage; }
	SCHEDULER_INLINE	CoreEntry*	PreviousCore() const;
	SCHEDULER_INLINE	CoreEntry*	Rebalance() const;

	SCHEDULER_INLINE	int32		GetEffectivePriority() const;

	SCHEDULER_INLINE	void		StartCPUTime();
	SCHEDULER_INLINE	void		StopCPUTime();

			void		ResetPriorityBoost();

			bool		ChooseCoreAndCPU(CoreEntry*& targetCore,
							CPUEntry*& targetCPU);

	SCHEDULER_INLINE	void		SetLastInterruptTime(bigtime_t interruptTime)
							{ fLastInterruptTime = interruptTime; }
	SCHEDULER_INLINE	void		SetStolenInterruptTime(bigtime_t interruptTime);

			bigtime_t	ComputeQuantum() const;
	SCHEDULER_INLINE	bigtime_t	GetQuantumLeft();
	SCHEDULER_INLINE	void		StartQuantum();
	SCHEDULER_INLINE	bool		HasQuantumEnded(bool wasPreempted, bool hasYielded);
			void		DonateTimesliceTo(Thread* beneficiary);

	SCHEDULER_INLINE	void		Continues();
	SCHEDULER_INLINE	void		GoesAway();
	SCHEDULER_INLINE	void		Dies();

	SCHEDULER_INLINE	bigtime_t	WentSleep() const	{ return fWentSleep; }
	SCHEDULER_INLINE	bigtime_t	WentSleepActive() const	{ return fWentSleepActive; }

	SCHEDULER_INLINE	void		PutBack();
	SCHEDULER_INLINE	bool		Enqueue(bool& wasRunQueueEmpty, bool& requestPreemption);
	SCHEDULER_INLINE	bool		Dequeue();

	// Accept an optional pre-computed 'now' timestamp.  The caller
	// (CPUEntry::UpdateActiveTime) already holds a fresh system_time() result
	// and can pass it here to eliminate a redundant syscall.  Passing 0 (the
	// default) falls back to an internal system_time() call for compatibility.
	SCHEDULER_INLINE	void		UpdateActivity(bigtime_t active,
								bigtime_t now = 0);

	static	bigtime_t	sMaxLatency;

	SCHEDULER_INLINE	bigtime_t	GetVirtualRuntime() const { return fVirtualRuntime; }

	SCHEDULER_INLINE	void		SetQuantum(bigtime_t quantum)
	{
		fBaseQuantum = quantum;
	}

	SCHEDULER_INLINE	bool		IsEnqueued() const	{ return fEnqueued; }
	SCHEDULER_INLINE	void		SetDequeued()
	{
		fEnqueued = false;
		fEnqueuedInCPURunQueue = false;
	}

	SCHEDULER_INLINE	int32		GetLoad() const	{ return fNeededLoad; }

	SCHEDULER_INLINE	bool		IsForeground() const	{ return fIsForeground; }
	SCHEDULER_INLINE	void		SetForeground(bool foreground)
	{
		fIsForeground = foreground;
	}

	SCHEDULER_INLINE	CoreEntry*	Core() const	{ return fCore; }
			void		UnassignCore(bool running = false);
			void		MigrateTo(CoreEntry* targetCore);

	static	void		ComputeQuantumLengths();

private:
	SCHEDULER_INLINE	void		_UpdatePriorityBoost();

			void		_ComputeNeededLoad();
			void		_UpdateDeadline();

			void		_ComputeEffectivePriority(bigtime_t now) const;

	static	bigtime_t	_ScaleQuantum(bigtime_t maxQuantum,
							bigtime_t minQuantum, int32 maxPriority,
							int32 minPriority, int32 priority);

			bigtime_t	fStolenTime;
			bigtime_t	fQuantumStart;
			bigtime_t	fLastInterruptTime;

			bigtime_t	fWentSleep;
			bigtime_t	fWentSleepActive;

			bool		fEnqueued;
			bool		fEnqueuedInCPURunQueue;
			bool		fReady;
			bool		fQuickStartCredit;
			bool		fIsForeground;
			bool		fStolen;

			Thread*		fThread;

			int32		fHomePackage;

	mutable	int32		fEffectivePriority;
	mutable	bigtime_t	fBaseQuantum;

			bigtime_t	fTimeUsed;

			bigtime_t	fMeasureAvailableActiveTime;
			bigtime_t	fMeasureAvailableTime;
			bigtime_t	fLastMeasureAvailableTime;

			int32		fNeededLoad;
			uint32		fLoadMeasurementEpoch;

			bigtime_t	fVirtualRuntime;
			bigtime_t	fVirtualDeadline;

			int32		fInteractivityScore;

			CoreEntry*	fCore;
};

class ThreadProcessing {
public:
	virtual				~ThreadProcessing();

	virtual	void		operator()(ThreadData* thread) = 0;
};


inline bool
ThreadData::IsRealTime() const
{
	return GetPriority() >= B_FIRST_REAL_TIME_PRIORITY;
}


inline bool
ThreadData::IsIdle() const
{
	return GetPriority() == B_IDLE_PRIORITY;
}


inline bool
ThreadData::HasCacheExpired() const
{
	SCHEDULER_ENTER_FUNCTION();
	return Scheduler::HasCacheExpired(this);
}


inline CoreEntry*
ThreadData::PreviousCore() const
{
	SCHEDULER_ENTER_FUNCTION();

	if (fThread->previous_cpu == NULL)
		return NULL;

	// Core() can transiently return NULL during hot-unplug: the CPUEntry's
	// fCore pointer is cleared before the CPU is fully removed from its
	// package.  Guard against this before dereferencing.
	CoreEntry* core = CPUEntry::GetCPU(fThread->previous_cpu->cpu_num)->Core();
	if (core == NULL || core->CPUCount() <= 0)
		return NULL;

	return core;
}


inline CoreEntry*
ThreadData::Rebalance() const
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!gSingleCore);
	return Scheduler::Rebalance(this);
}


inline int32
ThreadData::GetEffectivePriority() const
{
	SCHEDULER_ENTER_FUNCTION();
	return fEffectivePriority;
}


inline void
ThreadData::_UpdatePriorityBoost()
{
	SCHEDULER_ENTER_FUNCTION();

	if (IsIdle() || IsRealTime())
		return;

	int32 oldPriority = GetEffectivePriority();
	_ComputeEffectivePriority(system_time());
	int32 newPriority = GetEffectivePriority();

	if (oldPriority != newPriority) {
		if (fEnqueuedInCPURunQueue) {
			ASSERT(fThread->pinned_to_cpu > 0);
			CPUEntry* cpu = CPUEntry::GetCPU(fThread->previous_cpu->cpu_num);

			cpu->Remove(this);
			cpu->PushBack(this, newPriority);

			fEnqueuedInCPURunQueue = true;
		} else {
			//: Take the snapshot before Remove.
			CoreEntry* core = fCore;
			if (core != NULL) {
				core->Remove(this);
				CoreEntry* current = fCore;
				if (current != NULL && current != core) {
					// acquiring the new core's lock here is safe because
					// lock ordering requires Core-before-CPU and we hold no CPU
					// lock; the old core's lock is held by the caller.
					CoreRunQueueLocker newLock(current);
					current->PushBack(this, newPriority);
				} else
					core->PushBack(this, newPriority);

				// set fEnqueued immediately after PushBack while still
				// inside the critical section; the thread is in the queue now.
				fEnqueued = true;
			}

			fEnqueuedInCPURunQueue = false;
		}
	}
}


inline void
ThreadData::StartCPUTime()
{
	SCHEDULER_ENTER_FUNCTION();

	SpinLocker threadTimeLocker(fThread->time_lock);
	fThread->last_time = system_time();
}


inline void
ThreadData::StopCPUTime()
{
	SCHEDULER_ENTER_FUNCTION();

	// User time is tracked in thread_at_kernel_entry()
	SpinLocker threadTimeLocker(fThread->time_lock);
	fThread->kernel_time += system_time() - fThread->last_time;
	fThread->last_time = 0;
	threadTimeLocker.Unlock();

	// If the old thread's team has user time timers, check them now.
	Team* team = fThread->team;
	SpinLocker teamTimeLocker(team->time_lock);
	if (team->HasActiveUserTimeUserTimers())
		user_timer_check_team_user_timers(team);
}




inline void
ThreadData::SetStolenInterruptTime(bigtime_t interruptTime)
{
	SCHEDULER_ENTER_FUNCTION();

	bigtime_t delta = interruptTime - fLastInterruptTime;
	if (delta > 0)
		fStolenTime += delta;
}


inline bigtime_t
ThreadData::GetQuantumLeft()
{
	SCHEDULER_ENTER_FUNCTION();

	bigtime_t stolenTime = fStolenTime;
	fStolenTime = 0;

	bigtime_t quantum = ComputeQuantum() - fTimeUsed;
	quantum += stolenTime;
	quantum = max_c(quantum, Scheduler::MinimalQuantum());
	if (quantum > Scheduler::MaximumLatency())
		quantum = Scheduler::MaximumLatency();

	return quantum;
}


inline void
ThreadData::StartQuantum()
{
	SCHEDULER_ENTER_FUNCTION();
	fQuantumStart = system_time();
}


inline bool
ThreadData::HasQuantumEnded(bool wasPreempted, bool hasYielded)
{
	SCHEDULER_ENTER_FUNCTION();

	bigtime_t timeUsed = system_time() - fQuantumStart;
	ASSERT(timeUsed >= 0);
	fTimeUsed += timeUsed;

	bigtime_t timeLeft = ComputeQuantum() - fTimeUsed;
	timeLeft = max_c(bigtime_t(0), timeLeft);

	// too little time left, it's better make the next quantum a bit longer
	bigtime_t skipTime = Scheduler::MinimalQuantum() / 2;
	if (hasYielded) {
		timeLeft = 0;
		fInteractivityScore = min_c(fInteractivityScore + 20, 1000);
	} else if (wasPreempted || timeLeft <= skipTime) {
		fStolenTime += timeLeft;
		timeLeft = 0;

		if (!wasPreempted)
			fInteractivityScore = max_c(fInteractivityScore - 20, 0);
	}

	if (timeLeft == 0) {
		fTimeUsed = 0;
		_UpdateDeadline();
		return true;
	}

	return false;
}


inline void
ThreadData::Continues()
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(fReady);
	if (gTrackCoreLoad)
		_ComputeNeededLoad();
}


inline void
ThreadData::GoesAway()
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(fReady);

	if (!IsIdle()) {
		// the original atomic_set(0) could overwrite a concurrent
		// increment from another CPU that legitimately enqueued a thread in
		// the window between our atomic_add and the atomic_set, permanently
		// undercounting gTotalRunnableThreads.  Use a CAS retry loop that
		// only resets the counter while it is still negative, so valid
		// increments from concurrent CPUs are never lost.
		int32 prev = atomic_add(&gTotalRunnableThreads, -1);
		if (prev <= 0) {
			int32 cur = atomic_get(&gTotalRunnableThreads);
			while (cur < 0) {
				int32 was = atomic_test_and_set(&gTotalRunnableThreads, 0,
					cur);
				if (was == cur)
					break;
				cur = was;
			}
		}
	}

	if (!HasQuantumEnded(false, false)) {
		fQuickStartCredit = true;
		fInteractivityScore = min_c(fInteractivityScore + 10, 1000);
	}

	fLastInterruptTime = 0;

	//: Cache system_time() once; calling it twice gives slightly
	// different timestamps under heavy interrupt load, skewing sleep-time
	// accounting.  fWentSleepActive uses core active time, not wall time,
	// so no second call is needed for it.
	bigtime_t now = system_time();
	fWentSleep = now;
	fWentSleepActive = fCore->GetActiveTime();

	if (gTrackCoreLoad)
		fLoadMeasurementEpoch = fCore->RemoveLoad(fNeededLoad, false);
	fReady = false;
}


inline void
ThreadData::Dies()
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(fReady);

	if (!IsIdle()) {
		// same CAS-based saturation as GoesAway to avoid
		// overwriting concurrent increments.
		int32 prev = atomic_add(&gTotalRunnableThreads, -1);
		if (prev <= 0) {
			int32 cur = atomic_get(&gTotalRunnableThreads);
			while (cur < 0) {
				int32 was = atomic_test_and_set(&gTotalRunnableThreads, 0,
					cur);
				if (was == cur)
					break;
				cur = was;
			}
		}
	}

	if (gTrackCoreLoad)
		fCore->RemoveLoad(fNeededLoad, true);
	fReady = false;
}


inline void
ThreadData::PutBack()
{
	SCHEDULER_ENTER_FUNCTION();

	_ComputeEffectivePriority(system_time());
	int32 priority = GetEffectivePriority();

	if (fThread->pinned_to_cpu > 0) {
		ASSERT(fThread->cpu != NULL);
		CPUEntry* cpu = CPUEntry::GetCPU(fThread->cpu->cpu_num);

		// If the thread is pinned but we are running on a different CPU, it
		// means the pinned CPU was disabled. We should float until it comes back.
		if (fThread->cpu->cpu_num != fThread->pinned_to_cpu - 1)
			goto enqueue_core;

		CPURunQueueLocker _(cpu);
		ASSERT(!fEnqueued);
		fEnqueued = true;
		fEnqueuedInCPURunQueue = true;

		cpu->PushFront(this, priority);
	} else {
	enqueue_core:
		CoreRunQueueLocker _(fCore);
		ASSERT(!fEnqueued);
		fEnqueued = true;
		fEnqueuedInCPURunQueue = false;

		fCore->PushFront(this, priority);
	}
}


inline bool
ThreadData::Enqueue(bool& wasRunQueueEmpty, bool& requestPreemption)
{
	SCHEDULER_ENTER_FUNCTION();

	bool wasReady = fReady;
	if (!fReady) {
		if (gTrackCoreLoad) {
			bigtime_t timeSlept = system_time() - fWentSleep;
			bool updateLoad = timeSlept > 0;

			fCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, !updateLoad);
			if (updateLoad) {
				fMeasureAvailableTime += timeSlept;
				_ComputeNeededLoad();
			}
		}

		fReady = true;
	}

	fThread->state = B_THREAD_READY;

	const int32 priority = GetEffectivePriority();
	bool pinned = fThread->pinned_to_cpu > 0;
	if (pinned) {
		ASSERT(fThread->previous_cpu != NULL);
		CPUEntry* cpu = CPUEntry::GetCPU(fThread->previous_cpu->cpu_num);

		CPURunQueueLocker locker(cpu);

		// Check if the pinned CPU is disabled under the lock
		if (gCPU[cpu->ID()].disabled) {
			locker.Unlock();
			pinned = false; // float
		} else {
			if (!wasReady && !IsRealTime())
				_UpdateDeadline();

			// defer the gTotalRunnableThreads increment until after the
			// CPUCount guard in the non-pinned path (see below).  For the pinned
			// path the CPU liveness check happens under CPURunQueueLocker.
			if (!wasReady && !IsIdle())
				atomic_add(&gTotalRunnableThreads, 1);

			ASSERT(!fEnqueued);
			fEnqueued = true;
			fEnqueuedInCPURunQueue = true;

			ThreadData* top = cpu->PeekThread();
			wasRunQueueEmpty = (top == NULL || top->IsIdle());

			bool isForeground = fIsForeground;

			if (fQuickStartCredit || isForeground) {
				cpu->PushFront(this, priority);
				requestPreemption = true;
				if (isForeground)
					scheduler_update_interaction_state();
			} else
				cpu->PushBack(this, priority);
		}
	}

	if (!pinned) {
		CoreCPULocker cpuLocker(fCore);
		CoreRunQueueLocker locker(fCore);

		// _ChooseCore can return NULL (all masked CPUs disabled);
		// MigrateTo(NULL) sets fCore = NULL.  Guard before any deref.
		if (fCore == NULL)
			return false;

		// move the fStolen decrement AFTER the CPUCount guard so
		// that a return-false path never leaves TotalThreadCount decremented
		// without a corresponding enqueue to balance it.
		if (fStolen) {
			if (fCore->CPUCount() == 0) {
				fStolen = false;  // will be re-set by caller if re-stolen
				return false;
			}
			fCore->DecrementTotalThreadCount();
			fStolen = false;
		} else if (fCore->CPUCount() == 0) {
			return false;
		}

		if (!wasReady && !IsRealTime())
			_UpdateDeadline();

		// defer the gTotalRunnableThreads increment until after the
		// CPUCount guard in the non-pinned path.
		if (!wasReady && !IsIdle())
			atomic_add(&gTotalRunnableThreads, 1);

		ASSERT(!fEnqueued);
		fEnqueued = true;
		fEnqueuedInCPURunQueue = false;

		ThreadData* top = fCore->PeekThread();
		wasRunQueueEmpty = (top == NULL || top->IsIdle());

		bool isForeground = fIsForeground;

		if (fQuickStartCredit || isForeground) {
			fCore->PushFront(this, priority);
			requestPreemption = true;
			if (isForeground)
				scheduler_update_interaction_state();
		} else
			fCore->PushBack(this, priority);
	}
	fQuickStartCredit = false;
	return true;
}


inline bool
ThreadData::Dequeue()
{
	SCHEDULER_ENTER_FUNCTION();

	if (fEnqueuedInCPURunQueue) {
		ASSERT(fThread->previous_cpu != NULL);
		CPUEntry* cpu = CPUEntry::GetCPU(fThread->previous_cpu->cpu_num);

		CPURunQueueLocker _(cpu);
		if (!fEnqueued)
			return false;
		cpu->Remove(this);
		ASSERT(!fEnqueued);
		return true;
	}

	CoreRunQueueLocker _(fCore);
	if (!fEnqueued)
		return false;

	fCore->Remove(this);
	ASSERT(!fEnqueued);
	return true;
}


inline void
ThreadData::UpdateActivity(bigtime_t active, bigtime_t now)
{
	SCHEDULER_ENTER_FUNCTION();

	if (!IsRealTime()) {
		int32 priority = max_c((int32)1, GetEffectivePriority());
		bigtime_t delta = (active * B_URGENT_DISPLAY_PRIORITY) / priority;
		// Cap virtual runtime to a forward-looking ceiling rather than
		// B_INT64_MAX. A thread saturated at B_INT64_MAX would be permanently
		// starved because every new thread starts at fVirtualRuntime == 0.
		// With this ceiling the gap between a saturated thread and a new
		// thread is at most MaximumLatency()*1000 in virtual-time units,
		// after which they are scheduled fairly again as real time advances.
		// Use a monotonic base to prevent clock skew from causing starvation.
		const bigtime_t maxLatency = atomic_get64(&sMaxLatency);
		const bigtime_t kLookahead = maxLatency * 1000LL;
		if (now == 0)
			now = system_time();

		bigtime_t ceiling;
		if (now > B_INT64_MAX - kLookahead)
			ceiling = B_INT64_MAX;
		else
			ceiling = now + kLookahead;

		if (fVirtualRuntime < ceiling - delta)
			fVirtualRuntime += delta;
		else if (fVirtualRuntime < ceiling)
			fVirtualRuntime = ceiling;
		// If fVirtualRuntime is already at or above ceiling (e.g. ceiling moved
		// backward due to clock skew), leave it unchanged rather than reducing
		// it, which would spuriously boost the thread's scheduling priority.
	}

	if (!gTrackCoreLoad)
		return;

	fMeasureAvailableTime += active;
	fMeasureAvailableActiveTime += active;
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_THREAD_H

