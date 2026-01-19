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


struct ThreadData : public DoublyLinkedListLinkImpl<ThreadData>,
	RunQueueLinkImpl<ThreadData> {
private:
	inline	void		_InitBase();

	inline	int32		_GetMinimalPriority() const;

	inline	CoreEntry*	_ChooseCore() const;
	inline	CPUEntry*	_ChooseCPU(CoreEntry* core,
							bool& rescheduleNeeded) const;

public:
						ThreadData(Thread* thread);

			void		Init();
			void		Init(CoreEntry* core);

			void		Dump() const;

	inline	int32		GetPriority() const	{ return fThread->priority; }
	inline	Thread*		GetThread() const	{ return fThread; }
	inline	CPUSet		GetCPUMask() const	{ return fThread->cpumask.And(gCPUEnabled); }

	inline	bool		IsRealTime() const;
	inline	bool		IsIdle() const;

	inline	bool		HasCacheExpired() const;
	inline	CoreEntry*	Rebalance() const;

	inline	int32		GetEffectivePriority() const;

	inline	void		StartCPUTime();
	inline	void		StopCPUTime();

	inline	void		ResetPriorityBoost();

			bool		ChooseCoreAndCPU(CoreEntry*& targetCore,
							CPUEntry*& targetCPU);

	inline	void		SetLastInterruptTime(bigtime_t interruptTime)
							{ fLastInterruptTime = interruptTime; }
	inline	void		SetStolenInterruptTime(bigtime_t interruptTime);

			bigtime_t	ComputeQuantum() const;
	inline	bigtime_t	GetQuantumLeft();
	inline	void		StartQuantum();
	inline	bool		HasQuantumEnded(bool wasPreempted, bool hasYielded);
			void		DonateTimesliceTo(Thread* beneficiary);

	inline	void		Continues();
	inline	void		GoesAway();
	inline	void		Dies();

	inline	bigtime_t	WentSleep() const	{ return fWentSleep; }
	inline	bigtime_t	WentSleepActive() const	{ return fWentSleepActive; }

	inline	void		PutBack();
	inline	void		Enqueue(bool& wasRunQueueEmpty, bool& requestPreemption);
	inline	bool		Dequeue();

	inline	void		UpdateActivity(bigtime_t active);

	inline	bigtime_t	GetVirtualRuntime() const { return fVirtualRuntime; }

	inline	bool		IsEnqueued() const	{ return fEnqueued; }
	inline	void		SetDequeued()
	{
		fEnqueued = false;
		fEnqueuedInCPURunQueue = false;
	}

	inline	int32		GetLoad() const	{ return fNeededLoad; }

	inline	CoreEntry*	Core() const	{ return fCore; }
			void		UnassignCore(bool running = false);

	static	void		ComputeQuantumLengths();

private:
	inline	void		_UpdatePriorityBoost();
	inline	int32		_GetSkipCount() const;

			void		_ComputeNeededLoad();

			void		_ComputeEffectivePriority() const;

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

			int32		fSkipCount;
			int32		fPriorityBoost;

	mutable	int32		fEffectivePriority;
	mutable	bigtime_t	fBaseQuantum;

			bigtime_t	fTimeUsed;

			bigtime_t	fMeasureAvailableActiveTime;
			bigtime_t	fMeasureAvailableTime;
			bigtime_t	fLastMeasureAvailableTime;

			int32		fNeededLoad;
			uint32		fLoadMeasurementEpoch;

			bigtime_t	fVirtualRuntime;

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
	return std::max(std::min(priority, kMaximalPriority), kMinimalPriority);
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

	TRACE("increasing thread %ld skip count\n", fThread->id);

	int32 oldPriority = GetEffectivePriority();
	fSkipCount++;
	_ComputeEffectivePriority();
	int32 newPriority = GetEffectivePriority();

	if (oldPriority != newPriority) {
		bool inCPUQueue = fEnqueuedInCPURunQueue;
		if (inCPUQueue) {
			ASSERT(fThread->pinned_to_cpu > 0);
			CPUEntry* cpu = CPUEntry::GetCPU(fThread->previous_cpu->cpu_num);

			cpu->Remove(this);
			cpu->PushBack(this, newPriority);
		} else {
			fCore->Remove(this);
			fCore->PushBack(this, newPriority);
		}
		fEnqueued = true;
		fEnqueuedInCPURunQueue = inCPUQueue;
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

	fPriorityBoost = 0;
	fSkipCount = 0;
	_ComputeEffectivePriority();
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
	quantum = std::max(quantum, gCurrentMode->minimal_quantum);

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
	timeLeft = std::max(bigtime_t(0), timeLeft);

	// too little time left, it's better make the next quantum a bit longer
	bigtime_t skipTime = gCurrentMode->minimal_quantum / 2;
	if (hasYielded || wasPreempted || timeLeft <= skipTime) {
		fStolenTime += timeLeft;
		timeLeft = 0;
	}

	if (timeLeft == 0) {
		fTimeUsed = 0;
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

			if (!wasReady && !IsRealTime()) {
				bigtime_t minVirtualRuntime = cpu->GetMinVirtualRuntime();
				if (minVirtualRuntime > 0) {
					bigtime_t target = minVirtualRuntime - 2000;
					if (fVirtualRuntime < target)
						fVirtualRuntime = target;
				}
			}

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

		if (!wasReady && !IsRealTime()) {
			bigtime_t minVirtualRuntime = fCore->GetMinVirtualRuntime();
			if (minVirtualRuntime > 0) {
				bigtime_t target = minVirtualRuntime - 2000;
				if (fVirtualRuntime < target)
					fVirtualRuntime = target;
			}
		}

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
		int32 priority = std::max((int32)1, GetEffectivePriority());
		fVirtualRuntime += (active * B_URGENT_DISPLAY_PRIORITY) / priority;
	}

	if (!gTrackCoreLoad)
		return;

	fMeasureAvailableTime += active;
	fMeasureAvailableActiveTime += active;
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_THREAD_H

