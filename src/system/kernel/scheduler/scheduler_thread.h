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

struct ThreadData;

template <>
struct RunQueueTraits<ThreadData> {
	static inline void SetInRunQueue(ThreadData* element, bool inQueue);
};

struct CACHE_LINE_ALIGN ThreadData
	: public DoublyLinkedListLinkImpl<ThreadData> {
public:
	inline DoublyLinkedListLink<ThreadData>* GetRunQueueLink() { return &fRunQueueLink; }

private:
	inline void _InitBase();

	SCHEDULER_INLINE CoreEntry* _ChooseCore(const CPUSet& mask,
											bigtime_t now) const;
	SCHEDULER_INLINE CPUEntry* _ChooseCPU(CoreEntry* core,
										  bool& rescheduleNeeded) const;

public:
	ThreadData(Thread* thread);

	void Init(bigtime_t now = 0);
	void Init(CoreEntry* core);

	void Dump() const;

	SCHEDULER_INLINE int32 GetPriority() const { return fThread->priority; }
	SCHEDULER_INLINE Thread* GetThread() const { return fThread; }

	SCHEDULER_INLINE CPUSet GetCPUMask() const {
		CPUSet enabled;
		const int32 kWords = (SMP_MAX_CPUS + 31) / 32;
		for (int32 i = 0; i < kWords; i++) {
			uint32 w;
			int retry = 0;
			do {
				w = gCPUEnabled.Bits(i);
				memory_read_barrier();
				if (w == gCPUEnabled.Bits(i) || ++retry >= 3)
					break;
				cpu_pause();
			} while (true);
			enabled.SetWord(i, w);
		}
		return fThread->cpumask.And(enabled);
	}

	SCHEDULER_INLINE bool IsRealTime() const;
	SCHEDULER_INLINE bool IsIdle() const;

	SCHEDULER_INLINE bool HasCacheExpired(bigtime_t now = 0) const;
	SCHEDULER_INLINE int32 HomePackage() const { return fHomePackage; }
	SCHEDULER_INLINE CoreEntry* PreviousCore() const;
	SCHEDULER_INLINE CoreEntry* Rebalance(const CPUSet& mask,
										  bigtime_t now) const;

	SCHEDULER_INLINE int32 GetEffectivePriority() const;

	SCHEDULER_INLINE void StartCPUTime(bigtime_t now);
	SCHEDULER_INLINE void StartCPUTime() { StartCPUTime(system_time()); }
	SCHEDULER_INLINE void StopCPUTime(bigtime_t now);
	SCHEDULER_INLINE void StopCPUTime() { StopCPUTime(system_time()); }

	void ResetPriorityBoost(bigtime_t now = 0);

	bool ChooseCoreAndCPU(CoreEntry*& targetCore, CPUEntry*& targetCPU,
						  bigtime_t now = 0);

	SCHEDULER_INLINE void SetLastInterruptTime(bigtime_t interruptTime) {
		StoreRelease64(fLastInterruptTime, (int64)interruptTime);
	}
	SCHEDULER_INLINE void SetStolenInterruptTime(bigtime_t interruptTime);

	bigtime_t ComputeQuantum() const;
	SCHEDULER_INLINE bigtime_t GetQuantumLeft();
	SCHEDULER_INLINE void StartQuantum(bigtime_t now);
	SCHEDULER_INLINE void StartQuantum() { StartQuantum(system_time()); }
	SCHEDULER_INLINE bool HasQuantumEnded(bool wasPreempted, bool hasYielded,
										  bigtime_t now = 0);
	void DonateTimesliceTo(Thread* beneficiary, bigtime_t now = 0);

	SCHEDULER_INLINE void Continues(bigtime_t now = 0);
	SCHEDULER_INLINE void GoesAway(bigtime_t now = 0);
	SCHEDULER_INLINE void Dies(bigtime_t now = 0);

	SCHEDULER_INLINE bigtime_t WentSleep() const {
		return (bigtime_t)LoadAcquire64(fWentSleep);
	}

	SCHEDULER_INLINE bigtime_t WentSleepActive() const {
		return (bigtime_t)LoadAcquire64(fWentSleepActive);
	}

	SCHEDULER_INLINE void PutBack(bigtime_t now = 0);
	SCHEDULER_INLINE status_t CheckCapacity(CPUEntry* targetCPU);
	SCHEDULER_INLINE bool Enqueue(CPUEntry* targetCPU, bool& wasRunQueueEmpty,
								  bool& requestPreemption,
								  bool& updateInteraction, bigtime_t now = 0);
	SCHEDULER_INLINE bool Dequeue();

	SCHEDULER_INLINE void UpdateVirtualRuntime(bigtime_t delta,
											   bigtime_t svt,
											   bigtime_t maxLatency = 0);

	SCHEDULER_INLINE void UpdateActivity(bigtime_t active, bigtime_t svt,
										 bigtime_t now = 0);

	static bigtime_t sMaxLatency __attribute__((aligned(8)));

	SCHEDULER_INLINE bigtime_t GetVirtualRuntime() const {
		return (bigtime_t)LoadAcquire64(fThread->virtual_runtime);
	}

	SCHEDULER_INLINE bigtime_t GetVirtualDeadline() const {
		return (bigtime_t)LoadAcquire64(fThread->virtual_deadline);
	}

	SCHEDULER_INLINE int64 GetWeight() const {
		int64 weight = (int64)LoadAcquire(fThread->sched_weight);
		return (weight > 0) ? weight : 1;
	}

	SCHEDULER_INLINE int64 GetLag() const {
		return LoadAcquire64(fLag);
	}

	SCHEDULER_INLINE bool IsEligible(bigtime_t systemVirtualTime) const {
		return GetVirtualRuntime() <= systemVirtualTime;
	}

	SCHEDULER_INLINE void SetQuantum(bigtime_t quantum) {
		StoreRelease64(fBaseQuantum, (int64)quantum);
		StoreRelease64(fThread->time_slice, (int64)quantum);
	}

	SCHEDULER_INLINE bool IsEnqueued() const { return fEnqueued; }
	SCHEDULER_INLINE bool IsReady() const { return fReady; }
	SCHEDULER_INLINE void SetEnqueued(CPUEntry* cpu) {
		fEnqueued = true;
		fEnqueuedInCPURunQueue = true;
		atomic_pointer_set<CPUEntry>(&fEnqueuedCPU, cpu);
	}
	SCHEDULER_INLINE void SetDequeued() {
		fEnqueued = false;
		fEnqueuedInCPURunQueue = false;
		atomic_pointer_set<CPUEntry>(&fEnqueuedCPU, (CPUEntry*)NULL);
	}

	SCHEDULER_INLINE int32 GetLoad() const { return fNeededLoad; }

	SCHEDULER_INLINE int32 InteractivityScore() const {
		return fInteractivityScore;
	}

	SCHEDULER_INLINE bool IsForeground() const { return fIsForeground; }
	SCHEDULER_INLINE void SetForeground(bool foreground) {
		fIsForeground = foreground;
	}

	SCHEDULER_INLINE CoreEntry* Core() const {
		return atomic_pointer_get<CoreEntry>(
			const_cast<CoreEntry* volatile*>(&fCore));
	}
	SCHEDULER_INLINE CPUEntry* EnqueuedCPU() const {
		return atomic_pointer_get<CPUEntry>(
			const_cast<CPUEntry* volatile*>(&fEnqueuedCPU));
	}
	void UnassignCore(bool running = false);
	void MigrateTo(CoreEntry* targetCore, bigtime_t now = 0);

	static void ComputeQuantumLengths();

private:
	bigtime_t _ComputeQuantumForCore(CoreEntry* core,
									 scheduler_mode_operations* mode) const;

	SCHEDULER_INLINE void _UpdatePriorityBoost(bigtime_t now);

	void _ComputeNeededLoad(bigtime_t now = 0);
	void _UpdateDeadline(bigtime_t now = 0);

	void _ComputeEffectivePriority(bigtime_t now) const;

	static bigtime_t _ScaleQuantum(bigtime_t maxQuantum, bigtime_t minQuantum,
								   int32 maxPriority, int32 minPriority,
								   int32 priority);

	bigtime_t fStolenTime __attribute__((aligned(8)));
	bigtime_t fQuantumStart __attribute__((aligned(8)));
	bigtime_t fLastInterruptTime __attribute__((aligned(8)));

	bigtime_t fWentSleep __attribute__((aligned(8)));
	bigtime_t fWentSleepActive __attribute__((aligned(8)));

	bool fEnqueued;
	bool fEnqueuedInCPURunQueue;
	bool fReady;
	bool fQuickStartCredit;
	bool fIsForeground;
	bool fStolen;
	mutable bool fIsHighPriorityContributed;

	Thread* fThread;

	int32 fHomePackage __attribute__((aligned(8)));

	mutable int32 fEffectivePriority;
	mutable bigtime_t fBaseQuantum __attribute__((aligned(8)));

	bigtime_t fTimeUsed __attribute__((aligned(8)));

	bigtime_t fMeasureAvailableActiveTime __attribute__((aligned(8)));
	bigtime_t fMeasureAvailableTime __attribute__((aligned(8)));
	bigtime_t fLastMeasureAvailableTime __attribute__((aligned(8)));

	int32 fNeededLoad;
	uint32 fLoadMeasurementEpoch;

	bigtime_t fRequestSize __attribute__((aligned(8)));
	int64 fLag __attribute__((aligned(8)));

	int32 fInteractivityScore;

	DoublyLinkedListLink<ThreadData> fRunQueueLink;

	CoreEntry* fCore __attribute__((aligned(8)));
	CPUEntry* fEnqueuedCPU __attribute__((aligned(8)));
};

class ThreadProcessing {
public:
	virtual ~ThreadProcessing();

	virtual void operator()(ThreadData* thread) = 0;
};

inline bool ThreadData::IsRealTime() const {
	return GetPriority() >= B_FIRST_REAL_TIME_PRIORITY;
}

inline bool ThreadData::IsIdle() const {
	return GetPriority() == B_IDLE_PRIORITY;
}

inline bool ThreadData::HasCacheExpired(bigtime_t now) const {
	SCHEDULER_ENTER_FUNCTION();
	if (now == 0)
		now = system_time();
	return Scheduler::HasCacheExpired(this, now);
}

inline CoreEntry* ThreadData::PreviousCore() const {
	SCHEDULER_ENTER_FUNCTION();

	if (fThread->previous_cpu == NULL)
		return NULL;

	CoreEntry* core = CPUEntry::GetCPU(fThread->previous_cpu->cpu_num)->Core();
	if (core == NULL || core->CPUCount() <= 0)
		return NULL;

	return core;
}

inline CoreEntry* ThreadData::Rebalance(const CPUSet& mask,
										bigtime_t now) const {
	SCHEDULER_ENTER_FUNCTION();

	if (now == 0)
		now = system_time();

	ASSERT(!gSingleCore);
	return Scheduler::Rebalance(this, mask, now);
}

inline int32 ThreadData::GetEffectivePriority() const {
	SCHEDULER_ENTER_FUNCTION();
	return fEffectivePriority;
}

inline void ThreadData::_UpdatePriorityBoost(bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (IsIdle() || IsRealTime())
		return;

	int32 oldPriority = GetEffectivePriority();
	_ComputeEffectivePriority(now);
	int32 newPriority = GetEffectivePriority();

	if (oldPriority != newPriority) {
		CPUEntry* cpu = EnqueuedCPU();
		if (cpu != NULL) {
			// Note: lock is already held by caller (RunQueueScanner).
			cpu->Remove(this);
			cpu->PushBack(this, newPriority);
		}
	}
}

inline void ThreadData::StartCPUTime(bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	SpinLocker threadTimeLocker(fThread->time_lock);
	fThread->last_time = now;
}

inline void ThreadData::StopCPUTime(bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	// User time is tracked in thread_at_kernel_entry()
	SpinLocker threadTimeLocker(fThread->time_lock);
	fThread->kernel_time += now - fThread->last_time;
	fThread->last_time = 0;
	threadTimeLocker.Unlock();

	Team* team = fThread->team;
	SpinLocker teamTimeLocker(team->time_lock);
	if (team->HasActiveUserTimeUserTimers())
		user_timer_check_team_user_timers(team);
}

inline void ThreadData::SetStolenInterruptTime(bigtime_t interruptTime) {
	SCHEDULER_ENTER_FUNCTION();

	bigtime_t delta =
		interruptTime -
		(bigtime_t)LoadAcquire64(fLastInterruptTime);
	if (delta > 0) {
		AddRelease64(fStolenTime, (int64)delta);
	} else if (delta < 0) {
		dprintf("scheduler: interrupt_time went backward for thread %" B_PRId32
				" (delta %" B_PRId64 "); resetting baseline\n",
				fThread->id, delta);
	}
}

inline bigtime_t ThreadData::GetQuantumLeft() {
	SCHEDULER_ENTER_FUNCTION();

	bigtime_t stolenTime __attribute__((aligned(8)));
	do {
		stolenTime = (bigtime_t)LoadAcquire64(fStolenTime);
	} while ((bigtime_t)TestAndSet64(fStolenTime, 0, (int64)stolenTime) != stolenTime);

	bigtime_t quantum =
		ComputeQuantum() - (bigtime_t)LoadAcquire64(fTimeUsed);
	quantum += stolenTime;
	quantum = max_c(quantum, Scheduler::MinimalQuantum());
	if (quantum > Scheduler::MaximumLatency())
		quantum = Scheduler::MaximumLatency();

	return quantum;
}

inline void ThreadData::StartQuantum(bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();
	StoreRelease64(fQuantumStart, (int64)now);
}

inline bool ThreadData::HasQuantumEnded(bool wasPreempted, bool hasYielded,
										bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (now == 0)
		now = system_time();

	bigtime_t timeUsed =
		now - (bigtime_t)LoadAcquire64(fQuantumStart);
	ASSERT(timeUsed >= 0);
	bigtime_t timeUsedTotal =
		(bigtime_t)AddAcquireRelease64(fTimeUsed, (int64)timeUsed) +
		timeUsed;
	const bigtime_t kMaxTimeUsed = Scheduler::MaximumLatency() * 2;
	if (timeUsedTotal > kMaxTimeUsed) {
		timeUsedTotal = kMaxTimeUsed;
		StoreRelease64(fTimeUsed, (int64)kMaxTimeUsed);
	}

	bigtime_t quantum = ComputeQuantum();
	if (timeUsedTotal >= quantum) {
		StoreRelease64(fTimeUsed, 0);
		_UpdateDeadline(now);
		return true;
	}

	bigtime_t timeLeft = quantum - timeUsedTotal;
	timeLeft = max_c(bigtime_t(0), timeLeft);

	bigtime_t skipTime = Scheduler::MinimalQuantum() / 2;
	if (hasYielded) {
		timeLeft = 0;
		fInteractivityScore = min_c(fInteractivityScore + 20, 1000);
	} else if (wasPreempted || timeLeft <= skipTime) {
		AddRelease64(fStolenTime, (int64)timeLeft);
		timeLeft = 0;

		if (!wasPreempted) {
			fInteractivityScore =
				fInteractivityScore >= 20 ? fInteractivityScore - 20 : 0;
		}
	}

	if (timeLeft == 0) {
		StoreRelease64(fTimeUsed, 0);
		_UpdateDeadline(now);
		return true;
	}

	return false;
}

inline void ThreadData::Continues(bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (!fReady) {
		dprintf(
			"scheduler: Continues() called with fReady=false for thread "
			"%" B_PRId32
			" - possible GoesAway/reschedule race, skipping load update\n",
			fThread->id);
		return;
	}
	if (gTrackCoreLoad)
		_ComputeNeededLoad(now);
}

inline void ThreadData::GoesAway(bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(fReady);

	if (now == 0)
		now = system_time();

	if (!IsIdle()) {
		CPUEntry* cpu = EnqueuedCPU();
		if (cpu != NULL) {
			if (IsEnqueued())
				cpu->Remove(this);
			cpu->DecrementRunnableCount();
		}

		CoreEntry* const snap = atomic_pointer_get<CoreEntry>(
			const_cast<CoreEntry* volatile*>(&fCore));
		if (snap != NULL) {
			snap->DecrementTotalThreadCount();
			if (fIsHighPriorityContributed) {
				snap->DecrementHighPriorityThreadCount();
				fIsHighPriorityContributed = false;
			}
		}
	}

	if (!HasQuantumEnded(false, false, now)) {
		fQuickStartCredit = true;
		fInteractivityScore = min_c(fInteractivityScore + 10, 1000);
	}

	StoreRelease64(fLastInterruptTime, 0);

	StoreRelease64(fWentSleep, (int64)now);
	{
		CoreEntry* const snap = atomic_pointer_get<CoreEntry>(
			const_cast<CoreEntry* volatile*>(&fCore));
		StoreRelease64(fWentSleepActive, (int64)((snap != NULL) ? snap->GetActiveTime() : 0));
		if (gTrackCoreLoad && snap != NULL)
			fLoadMeasurementEpoch = snap->RemoveLoad(fNeededLoad, false, now);
	}
	fReady = false;
}

inline void ThreadData::Dies(bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(fReady);

	if (now == 0)
		now = system_time();

	if (!IsIdle()) {
		CPUEntry* cpu = EnqueuedCPU();
		if (cpu != NULL) {
			if (IsEnqueued())
				cpu->Remove(this);
			cpu->DecrementRunnableCount();
		}

		CoreEntry* const snap = atomic_pointer_get<CoreEntry>(
			const_cast<CoreEntry* volatile*>(&fCore));
		if (snap != NULL) {
			snap->DecrementTotalThreadCount();
			if (fIsHighPriorityContributed) {
				snap->DecrementHighPriorityThreadCount();
				fIsHighPriorityContributed = false;
			}
		}
	}

	if (gTrackCoreLoad) {
		CoreEntry* const snap = atomic_pointer_get<CoreEntry>(&fCore);
		if (snap != NULL)
			snap->RemoveLoad(fNeededLoad, true, now);
	}
	fReady = false;
}

inline void ThreadData::PutBack(bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (now == 0)
		now = system_time();

	_ComputeEffectivePriority(now);
	int32 priority = GetEffectivePriority();

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());

	if (CheckCapacity(cpu) != B_OK) {
		panic("scheduler: capacity exceeded in PutBack for thread %" B_PRId32,
			fThread->id);
	}

	CPURunQueueLocker _(cpu);
	ASSERT(!fEnqueued);
	if (!fReady) {
		fReady = true;
		fThread->state = B_THREAD_READY;

		if (!IsIdle()) {
			cpu->IncrementRunnableCount();
			CoreEntry* core = cpu->Core();
			if (core != NULL) {
				core->IncrementTotalThreadCount();
				if (priority >= B_DISPLAY_PRIORITY) {
					core->IncrementHighPriorityThreadCount();
					fIsHighPriorityContributed = true;
				}
			}
		}
	}

	cpu->PushFront(this, priority);
}

inline status_t ThreadData::CheckCapacity(CPUEntry* targetCPU) {
	if (targetCPU == NULL)
		return B_OK;
	return const_cast<ThreadRunQueue*>(targetCPU->RunQueue())->CheckCapacity(targetCPU->ThreadCount() + 1);
}

inline bool ThreadData::Enqueue(CPUEntry* cpu, bool& wasRunQueueEmpty,
								bool& requestPreemption,
								bool& updateInteraction, bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (now == 0)
		now = system_time();

	updateInteraction = false;

	bool wasReady = fReady;

	if (CheckCapacity(cpu) != B_OK)
		return false;

	const int32 priority = GetEffectivePriority();

	CPURunQueueLocker locker(cpu);

	if (gCPU[cpu->ID()].disabled)
		return false;

	CoreEntry* core = cpu->Core();

	if (fStolen) {
		if (gTrackCoreLoad && !wasReady) {
			bigtime_t timeSlept =
				now - (bigtime_t)LoadAcquire64(fWentSleep);
			bool updateLoad = timeSlept > 0;
			core->AddLoad(fNeededLoad, fLoadMeasurementEpoch, !updateLoad,
						  now);
			if (updateLoad) {
				AddRelease64(fMeasureAvailableTime, (int64)timeSlept);
				_ComputeNeededLoad(now);
			}
		}
		fStolen = false;
	} else if (!wasReady && gTrackCoreLoad) {
		bigtime_t timeSlept =
			now - (bigtime_t)LoadAcquire64(fWentSleep);
		bool updateLoad = timeSlept > 0;
		core->AddLoad(fNeededLoad, fLoadMeasurementEpoch, !updateLoad, now);
		if (updateLoad) {
			AddRelease64(fMeasureAvailableTime, (int64)timeSlept);
			_ComputeNeededLoad(now);
		}
	}

	if (!wasReady && !IsRealTime()) {
		bigtime_t svt = cpu->SystemVirtualTime();
		bigtime_t vrt = GetVirtualRuntime();

		int64 weight = GetWeight();
		if (weight <= 0)
			weight = 1;
		bigtime_t vLagFloor = (kMaxLagFloor * 1000000LL) / weight;

		if (vrt < svt - vLagFloor)
			StoreRelease64(fThread->virtual_runtime, (int64)(svt - vLagFloor));

		_UpdateDeadline(now);
	}

	if (!wasReady) {
		if (!IsIdle()) {
			cpu->IncrementRunnableCount();
			if (core != NULL) {
				core->IncrementTotalThreadCount();
				if (priority >= B_DISPLAY_PRIORITY) {
					core->IncrementHighPriorityThreadCount();
					fIsHighPriorityContributed = true;
				}
			}
		}

		fReady = true;
		fThread->state = B_THREAD_READY;
	}

	ASSERT(!fEnqueued);
	SetEnqueued(cpu);

	ThreadData* top = cpu->PeekThread();
	wasRunQueueEmpty = (top == NULL || top->IsIdle());

	bool isForeground = fIsForeground;

	if (fQuickStartCredit || isForeground) {
		cpu->PushFront(this, priority);
		requestPreemption = true;
		if (isForeground)
			updateInteraction = true;
	} else
		cpu->PushBack(this, priority);

	fQuickStartCredit = false;
	return true;
}

inline bool ThreadData::Dequeue() {
	SCHEDULER_ENTER_FUNCTION();

	CPUEntry* cpu = EnqueuedCPU();
	if (cpu == NULL)
		return false;

	CPURunQueueLocker _(cpu);
	if (!fEnqueued)
		return false;

	cpu->Remove(this);
	ASSERT(!fEnqueued);
	atomic_pointer_set<CPUEntry>(&fEnqueuedCPU, (CPUEntry*)NULL);
	return true;
}

inline void RunQueueTraits<ThreadData>::SetInRunQueue(ThreadData* element,
													  bool inQueue) {
	element->GetThread()->inRunQueue = inQueue;
}

inline void ThreadData::UpdateVirtualRuntime(bigtime_t delta, bigtime_t svt,
											 bigtime_t maxLatency) {
	if (delta <= 0 || IsRealTime())
		return;

	if (maxLatency == 0) {
		maxLatency = (bigtime_t)LoadAcquire64(sMaxLatency);
		if (maxLatency == 0)
			maxLatency = 3200;
	}
	const bigtime_t kLookahead = 300000000000LL;

	if (svt == 0)
		svt = system_time() * 1000;

	bigtime_t ceiling =
		(svt > B_INT64_MAX - kLookahead) ? B_INT64_MAX : svt + kLookahead;

	// Note: 32-bit Torn Read Guard.
	// On 32-bit platforms, LoadAcquire64 might not be natively atomic.
	// Although scheduler_common.h provides a wrapper, we use the CAS
	// result itself as the authoritative source of the current value to
	// ensure the update loop never operates on a torn snapshot.
	bigtime_t vRuntime = (bigtime_t)LoadAcquire64(fThread->virtual_runtime);
	while (vRuntime < ceiling) {
		bigtime_t next = (vRuntime < ceiling - delta) ? vRuntime + delta : ceiling;

		bigtime_t old = (bigtime_t)TestAndSet64(fThread->virtual_runtime, (int64)next,
												(int64)vRuntime);
		if (old == vRuntime)
			break;

		// Note: snapshot consistency.
		// If TestAndSet64 fails, it returns the *actual* current value.
		// On 32-bit targets, if the first LoadAcquire64 was torn, this
		// assignment from 'old' corrects it for the next iteration.
		vRuntime = old;
	}
}

inline void ThreadData::UpdateActivity(bigtime_t active, bigtime_t svt,
									   bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (!IsRealTime() && !IsIdle()) {
		int64 weight = GetWeight();
		if (weight <= 0)
			weight = 1;
		bigtime_t delta = (active * 1000000LL) / weight;

		UpdateVirtualRuntime(delta, svt);

		int64 lag = (svt - GetVirtualRuntime()) * weight / 1000;
		StoreRelease64(fLag, lag);
	}

	if (!gTrackCoreLoad)
		return;

	AddRelease64(fMeasureAvailableTime, (int64)active);
	AddRelease64(fMeasureAvailableActiveTime, (int64)active);
}

}  // namespace Scheduler

#endif	// KERNEL_SCHEDULER_THREAD_H
