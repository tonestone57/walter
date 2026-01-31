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


const bigtime_t kPriorityBoostInterval = 300000;


struct CACHE_LINE_ALIGN ThreadData : public DoublyLinkedListLinkImpl<ThreadData>,
	RunQueueLinkImpl<ThreadData> {
private:
	inline	void		_InitBase();

	SCHEDULER_INLINE	int32		_GetMinimalPriority() const;

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
	SCHEDULER_INLINE	CPUSet		GetCPUMask() const	{ return fThread->cpumask.And(gCPUEnabled); }

	SCHEDULER_INLINE	bool		IsRealTime() const;
	SCHEDULER_INLINE	bool		IsIdle() const;

	SCHEDULER_INLINE	bool		HasCacheExpired() const;
	SCHEDULER_INLINE	int32		HomePackage() const { return fHomePackage; }
	SCHEDULER_INLINE	CoreEntry*	Rebalance() const;

	SCHEDULER_INLINE	int32		GetEffectivePriority() const;

	SCHEDULER_INLINE	void		StartCPUTime();
	SCHEDULER_INLINE	void		StopCPUTime();

	SCHEDULER_INLINE	void		ResetPriorityBoost();

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
	SCHEDULER_INLINE	void		Enqueue(bool& wasRunQueueEmpty, bool& requestPreemption);
	SCHEDULER_INLINE	bool		Dequeue();

	SCHEDULER_INLINE	void		UpdateActivity(bigtime_t active);

	SCHEDULER_INLINE	bigtime_t	GetVirtualRuntime() const { return fVirtualRuntime; }

	SCHEDULER_INLINE	bool		IsEnqueued() const	{ return fEnqueued; }
	SCHEDULER_INLINE	void		SetDequeued()
	{
		fEnqueued = false;
		fEnqueuedInCPURunQueue = false;
	}

	SCHEDULER_INLINE	int32		GetLoad() const	{ return fNeededLoad; }

	SCHEDULER_INLINE	CoreEntry*	Core() const	{ return fCore; }
			void		UnassignCore(bool running = false);

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

			CoreEntry*	fCore;
};

class ThreadProcessing {
public:
	virtual				~ThreadProcessing();

	virtual	void		operator()(ThreadData* thread) = 0;
};


inline int32
ThreadData::_GetMinimalPriority() const
{
	SCHEDULER_ENTER_FUNCTION();

	const int32 kDivisor = 5;

	const int32 kMaximalPriority = 25;
	const int32 kMinimalPriority = B_LOWEST_ACTIVE_PRIORITY;

	int32 priority = GetPriority() / kDivisor;
	return max_c(min_c(priority, kMaximalPriority), kMinimalPriority);
}


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
	return gCurrentMode->has_cache_expired(this);
}


inline CoreEntry*
ThreadData::Rebalance() const
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!gSingleCore);
	return gCurrentMode->rebalance(this);
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
			fCore->Remove(this);
			fCore->PushBack(this, newPriority);

			fEnqueuedInCPURunQueue = false;
		}
		fEnqueued = true;
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
ThreadData::ResetPriorityBoost()
{
	SCHEDULER_ENTER_FUNCTION();

	_ComputeEffectivePriority(system_time());
}


inline void
ThreadData::SetStolenInterruptTime(bigtime_t interruptTime)
{
	SCHEDULER_ENTER_FUNCTION();

	interruptTime -= fLastInterruptTime;
	fStolenTime += interruptTime;
}


inline bigtime_t
ThreadData::GetQuantumLeft()
{
	SCHEDULER_ENTER_FUNCTION();

	bigtime_t stolenTime = fStolenTime;
	fStolenTime = 0;

	bigtime_t quantum = ComputeQuantum() - fTimeUsed;
	quantum += stolenTime;
	quantum = max_c(quantum, gCurrentMode->minimal_quantum);

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
	bigtime_t skipTime = gCurrentMode->minimal_quantum / 2;
	if (hasYielded || wasPreempted || timeLeft <= skipTime) {
		fStolenTime += timeLeft;
		timeLeft = 0;
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

	if (!HasQuantumEnded(false, false))
		fQuickStartCredit = true;

	fLastInterruptTime = 0;

	fWentSleep = system_time();
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


inline void
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

		// If the pinned CPU is disabled, we treat the thread as unpinned
		// temporarily and let it float in the Core run queue.
		if (gCPU[cpu->ID()].disabled) {
			pinned = false;
		} else {
			CPURunQueueLocker _(cpu);

			if (!wasReady && !IsRealTime())
				_UpdateDeadline();

			ASSERT(!fEnqueued);
			fEnqueued = true;
			fEnqueuedInCPURunQueue = true;

			ThreadData* top = cpu->PeekThread();
			wasRunQueueEmpty = (top == NULL || top->IsIdle());

			if (fQuickStartCredit) {
				cpu->PushFront(this, priority);
				requestPreemption = true;
			} else
				cpu->PushBack(this, priority);
		}
	}

	if (!pinned) {
		CoreRunQueueLocker _(fCore);

		if (!wasReady && !IsRealTime())
			_UpdateDeadline();

		ASSERT(!fEnqueued);
		fEnqueued = true;
		fEnqueuedInCPURunQueue = false;

		ThreadData* top = fCore->PeekThread();
		wasRunQueueEmpty = (top == NULL || top->IsIdle());

		if (fQuickStartCredit) {
			fCore->PushFront(this, priority);
			requestPreemption = true;
		} else
			fCore->PushBack(this, priority);
	}
	fQuickStartCredit = false;
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
ThreadData::UpdateActivity(bigtime_t active)
{
	SCHEDULER_ENTER_FUNCTION();

	if (!IsRealTime()) {
		int32 priority = max_c((int32)1, GetEffectivePriority());
		fVirtualRuntime += (active * B_URGENT_DISPLAY_PRIORITY) / priority;
	}

	if (!gTrackCoreLoad)
		return;

	fMeasureAvailableTime += active;
	fMeasureAvailableActiveTime += active;
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_THREAD_H

