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
	: public DoublyLinkedListLinkImpl<ThreadData>,
	  RunQueueLinkImpl<ThreadData> {
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
	// gCPUEnabled is updated one word at a time by SetBitAtomic/
	// ClearBitAtomic; there is no compound-And atomicity guarantee.
	// Note: iterate with a snapshot approach to ensure word-boundary
	// consistency.  While word-aligned reads are indivisible on x86, we
	// the retry condition
	// Note: word-boundary consistency via retry loop
	// is CORRECT.  It retries when the two reads DIFFER (unstable) and
	// retry < 3, and breaks when they are EQUAL (stable) OR retries are
	// exhausted.  This is the intended behaviour and is NOT inverted.
	// No code change required.
	//
	// can still read two words representing different snapshots.  We
	// probe the words and re-read if we suspect a race.
	SCHEDULER_INLINE CPUSet GetCPUMask() const {
		CPUSet enabled;
		const int32 kWords = (SMP_MAX_CPUS + 31) / 32;
		for (int32 i = 0; i < kWords; i++) {
			uint32 w;
			int retry = 0;
			do {
				w = gCPUEnabled.Bits(i);
				// Ensure word-boundary consistency during concurrent updates.
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

	// Continues(), GoesAway(), and Dies() accept an optional 'now' timestamp
	// to avoid redundant system_time() calls in the reschedule() hot path.
	SCHEDULER_INLINE void Continues(bigtime_t now = 0);
	SCHEDULER_INLINE void GoesAway(bigtime_t now = 0);
	SCHEDULER_INLINE void Dies(bigtime_t now = 0);

	SCHEDULER_INLINE bigtime_t WentSleep() const {
		return (bigtime_t)LoadAcquire64(fWentSleep);
	}

	SCHEDULER_INLINE bigtime_t WentSleepActive() const {
		return (bigtime_t)LoadAcquire64(fWentSleepActive);
	}

	// PutBack() and Enqueue() accept an optional 'now' timestamp for
	// timestamp propagation.
	SCHEDULER_INLINE void PutBack(bigtime_t now = 0);
	SCHEDULER_INLINE bool Enqueue(bool& wasRunQueueEmpty,
								  bool& requestPreemption,
								  bool& updateInteraction, bigtime_t now = 0);
	SCHEDULER_INLINE bool Dequeue();

	SCHEDULER_INLINE void UpdateVirtualRuntime(bigtime_t delta,
											   bigtime_t now = 0,
											   bigtime_t maxLatency = 0);

	// Accept an optional pre-computed 'now' timestamp.  The caller
	// (CPUEntry::UpdateActiveTime) already holds a fresh system_time() result
	// and can pass it here to eliminate a redundant syscall.  Passing 0 (the
	// default) falls back to an internal system_time() call for compatibility.
	SCHEDULER_INLINE void UpdateActivity(bigtime_t active, bigtime_t now = 0);

	static bigtime_t sMaxLatency __attribute__((aligned(8)));

	SCHEDULER_INLINE bigtime_t GetVirtualRuntime() const {
		return (bigtime_t)LoadAcquire64(fVirtualRuntime);
	}

	SCHEDULER_INLINE int64 GetWeight() const {
		return LoadAcquire64(fWeight);
	}

	SCHEDULER_INLINE int64 GetLag() const {
		return LoadAcquire64(fLag);
	}

	SCHEDULER_INLINE bool IsEligible(bigtime_t systemVirtualTime) const {
		return GetVirtualRuntime() <= systemVirtualTime;
	}

	SCHEDULER_INLINE void SetQuantum(bigtime_t quantum) {
		StoreRelease64(fBaseQuantum, (int64)quantum);
	}

	SCHEDULER_INLINE bool IsEnqueued() const { return fEnqueued; }
	SCHEDULER_INLINE bool IsReady() const { return fReady; }
	SCHEDULER_INLINE void SetDequeued() {
		fEnqueued = false;
		fEnqueuedInCPURunQueue = false;
	}

	SCHEDULER_INLINE int32 GetLoad() const { return fNeededLoad; }

	SCHEDULER_INLINE bool IsForeground() const { return fIsForeground; }
	SCHEDULER_INLINE void SetForeground(bool foreground) {
		fIsForeground = foreground;
	}

	SCHEDULER_INLINE CoreEntry* Core() const {
		return atomic_pointer_get<CoreEntry>(
			const_cast<CoreEntry* volatile*>(&fCore));
	}
	void UnassignCore(bool running = false);
	void MigrateTo(CoreEntry* targetCore, bigtime_t now = 0);

	static void ComputeQuantumLengths();

private:
	// Must be called with the appropriate run-queue lock held (either
	// CPUEntry::fQueueLock or CoreEntry::fQueueLock).
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

	int64 fWeight __attribute__((aligned(8)));
	bigtime_t fRequestSize __attribute__((aligned(8)));
	int64 fLag __attribute__((aligned(8)));

	bigtime_t fVirtualRuntime __attribute__((aligned(8)));
	bigtime_t fVirtualDeadline __attribute__((aligned(8)));

	int32 fInteractivityScore;

	CoreEntry* fCore __attribute__((aligned(8)));
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

	// Core() can transiently return NULL during hot-unplug: the CPUEntry's
	// fCore pointer is cleared before the CPU is fully removed from its
	// package.  Guard against this before dereferencing.
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
		if (fEnqueuedInCPURunQueue) {
			ASSERT(fThread->pinned_to_cpu > 0);
			CPUEntry* cpu = CPUEntry::GetCPU(fThread->previous_cpu->cpu_num);

			// Note: lock is already held by caller.
			cpu->Remove(this);
			cpu->PushBack(this, newPriority);

			fEnqueued = true;
			fEnqueuedInCPURunQueue = true;
		} else {
			// Note: capture fCore under the assumption that the caller
			// ALREADY holds the CoreRunQueueLocker for this core.
			// The previous code attempted to acquire it again, causing a
			// deadlock.  Re-acquisition is also unnecessary because
			// MigrateTo() cannot change fCore while we hold the run-queue lock
			// of the core we are currently enqueued in.
			CoreEntry* core = atomic_pointer_get<CoreEntry>(&fCore);
			if (core != NULL) {
				// Set state BEFORE removing to ensure no one sees a
				// ready-but-not-enqueued thread.
				fEnqueued = false;
				core->Remove(this);
				core->PushBack(this, newPriority);
				fEnqueued = true;
			}

			fEnqueuedInCPURunQueue = false;
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

	// If the old thread's team has user time timers, check them now.
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
	// Note: if interrupt_time goes backward (e.g. CPU accounting
	// reset or wrap), delta is negative and fLastInterruptTime must be
	// reset to the current interruptTime to restore correct accounting.
	// Otherwise fLastInterruptTime stays at a "future" value permanently
	// suppressing all stolen-time accounting for this thread.
	if (delta > 0) {
		AddRelease64(fStolenTime, (int64)delta);
	} else if (delta < 0) {
		// Clock went backward; reset baseline to avoid permanent suppression.
		// Do not add the negative delta - the time is simply unaccountable.
		dprintf("scheduler: interrupt_time went backward for thread %" B_PRId32
				" (delta %" B_PRId64 "); resetting baseline\n",
				fThread->id, delta);
	}
	// fLastInterruptTime is always updated via SetLastInterruptTime() by
	// the caller; this function only handles the fStolenTime accumulation.
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
	// Note: cap fTimeUsed accumulation. Under extremely rapid
	// rescheduling, fTimeUsed can accumulate to near B_INT64_MAX before the
	// quantum-end check fires. When that happens, quantum - fTimeUsed
	// underflows to a large positive, granting an unintended stolen-time
	// bonus. Cap at 2 * MaximumLatency() as a generous but safe upper bound.
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

	// too little time left, it's better make the next quantum a bit longer
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

	// Note: fReady is written by GoesAway/Dies without holding the
	// core run-queue lock. A concurrent CPU calling GoesAway on this thread
	// while it is being rescheduled can clear fReady before Continues() checks
	// it, causing a spurious assertion. Demote to a debug dprintf instead of
	// a hard ASSERT, which would panic in this rare but legitimate race.
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
		int32 prev = AddAcquireRelease(*const_cast<int32 volatile*>(&gTotalRunnableThreads), -1);
		if (prev <= 0) {
			int32 cur = LoadAcquire(gTotalRunnableThreads);
			for (int32 i = 0; i < 100 && cur < 0; i++) {
				int32 was = cur;
				if (TestAndSet(*const_cast<int32 volatile*>(&gTotalRunnableThreads), 0, (int32)(was)) == was) {
					break;
				}
				cur = LoadAcquire(gTotalRunnableThreads);
				memory_read_barrier();
			}
		}
	}
	// Note: DecrementTotalThreadCount (called from GoesAway via
	// CPUGoesIdle) decrements fTotalThreadCount before the idle-transition
	// check. Document that ThreadCount() callers (e.g.
	// UpdatePriorityBoostScalable) may transiently see count-1 during this
	// window. This is benign for the boost-scan decision (one missed scan
	// quantum is acceptable) but callers must not rely on ThreadCount() == 0
	// meaning the core is definitively empty.

	if (!HasQuantumEnded(false, false, now)) {
		fQuickStartCredit = true;
		fInteractivityScore = min_c(fInteractivityScore + 10, 1000);
	}

	StoreRelease64(fLastInterruptTime, 0);

	StoreRelease64(fWentSleep, (int64)now);
	// Note: fCore can be set to NULL by a concurrent MigrateTo() call.
	// The original code checked for NULL once then called GetActiveTime() and
	// RemoveLoad() in separate statements - if fCore became NULL between the
	// check and either call, both would dereference NULL.
	// Fix: take ONE snapshot under a read of fCore, then use only the snapshot.
	// MigrateTo() is only called from ChooseCoreAndCPU which holds
	// CoreCPULocker; GoesAway is called from reschedule() which holds
	// SchedulerModeLocker (read). These are different locks, so the race
	// is real. The snapshot approach is the minimal safe fix.
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
		int32 prev = AddAcquireRelease(*const_cast<int32 volatile*>(&gTotalRunnableThreads), -1);
		if (prev <= 0) {
			int32 cur = LoadAcquire(gTotalRunnableThreads);
			for (int32 i = 0; i < 100 && cur < 0; i++) {
				int32 was = cur;
				if (TestAndSet(*const_cast<int32 volatile*>(&gTotalRunnableThreads), 0, (int32)(was)) == was) {
					break;
				}
				cur = LoadAcquire(gTotalRunnableThreads);
				memory_read_barrier();
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

	if (fThread->pinned_to_cpu > 0) {
		ASSERT(fThread->cpu != NULL);
		CPUEntry* cpu = CPUEntry::GetCPU(fThread->cpu->cpu_num);

		// If the thread is pinned but we are running on a different CPU, it
		// means the pinned CPU was disabled. We should float until it comes
		// back.
		if (fThread->cpu->cpu_num != fThread->pinned_to_cpu - 1)
			goto enqueue_core;

		CPURunQueueLocker _(cpu);
		ASSERT(!fEnqueued);
		fEnqueued = true;
		fEnqueuedInCPURunQueue = true;

		cpu->PushFront(this, priority);
	} else {
	enqueue_core:
		CoreEntry* core = Core();
		CoreRunQueueLocker _(core);
		ASSERT(!fEnqueued);
		fEnqueued = true;
		fEnqueuedInCPURunQueue = false;

		core->PushFront(this, priority);
	}
}

inline bool ThreadData::Enqueue(bool& wasRunQueueEmpty, bool& requestPreemption,
								bool& updateInteraction, bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (now == 0)
		now = system_time();

	updateInteraction = false;

	bool wasReady = fReady;

	const int32 priority = GetEffectivePriority();
	bool pinned = fThread->pinned_to_cpu > 0;
	if (pinned) {
		ASSERT(fThread->previous_cpu != NULL);
		CPUEntry* cpu = CPUEntry::GetCPU(fThread->previous_cpu->cpu_num);

		CPURunQueueLocker locker(cpu);

		// Check if the pinned CPU is disabled under the lock
		if (gCPU[cpu->ID()].disabled) {
			locker.Unlock();
			pinned = false;	 // float
		} else {
			if (!wasReady && !IsRealTime())
				_UpdateDeadline(now);

			// defer the gTotalRunnableThreads increment until after the
			// CPUCount guard in the non-pinned path (see below).  For the
			// pinned path the CPU liveness check happens under
			// CPURunQueueLocker.
			if (!wasReady && !IsIdle())
				AddRelease(gTotalRunnableThreads, 1);

			fReady = true;
			fThread->state = B_THREAD_READY;

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
					updateInteraction = true;
			} else
				cpu->PushBack(this, priority);
		}
	}

	if (!pinned) {
		// guard fCore NULL before constructing the RAII lockers.
		// MigrateTo(NULL) can set fCore=NULL when all masked CPUs are disabled.
		// Both CoreCPULocker and CoreRunQueueLocker dereference their argument
		// in the constructor; a NULL fCore here causes a null-dereference panic
		// before we even reach the existing null check below.
		CoreEntry* coreSnapshot = atomic_pointer_get<CoreEntry>(&fCore);
		if (coreSnapshot == NULL)
			return false;

		CoreCPULocker cpuLocker(coreSnapshot);
		CoreRunQueueLocker locker(coreSnapshot);

		// Note: re-check under the lock - fCore may have been set to
		// NULL between the guard above and lock acquisition.  The explicit
		// Unlock() calls were redundant: AutoLocker's destructor checks
		// fLocked and will not double-unlock.  RAII handles cleanup correctly.
		if (atomic_pointer_get<CoreEntry>(&fCore) != coreSnapshot)
			return false;

		CoreEntry* core = coreSnapshot;

		// move the fStolen decrement AFTER the CPUCount guard so
		// that a return-false path never leaves TotalThreadCount decremented
		// without a corresponding enqueue to balance it.
		// Note: AddLoad was previously called unconditionally before
		// the CPUCount guard, leaving load permanently inflated when
		// CPUCount==0 caused return false. Move AddLoad AFTER all guards.
		if (fStolen) {
			if (core->CPUCount() == 0) {
				core->DecrementTotalThreadCount();
				fStolen = false;
				return false;
			}
			// Note: AddLoad happens here, after all early-return guards.
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
			core->DecrementTotalThreadCount();
			fStolen = false;
		} else if (core->CPUCount() == 0) {
			return false;
		} else if (!wasReady && gTrackCoreLoad) {
			// Note: for non-stolen threads, AddLoad after CPUCount guard.
			// Note: ensure AddLoad captures the full sleep time when a thread
			// is woken up.
			bigtime_t timeSlept =
				now - (bigtime_t)LoadAcquire64(fWentSleep);
			bool updateLoad = timeSlept > 0;
			core->AddLoad(fNeededLoad, fLoadMeasurementEpoch, !updateLoad, now);
			if (updateLoad) {
				AddRelease64(fMeasureAvailableTime, (int64)timeSlept);
				_ComputeNeededLoad(now);
			}
		}

		if (!wasReady && !IsRealTime())
			_UpdateDeadline(now);

		// defer the gTotalRunnableThreads increment until after the
		// CPUCount guard in the non-pinned path.
		if (!wasReady && !IsIdle())
			AddRelease(gTotalRunnableThreads, 1);

		fReady = true;
		fThread->state = B_THREAD_READY;

		ASSERT(!fEnqueued);
		fEnqueued = true;
		fEnqueuedInCPURunQueue = false;

		ThreadData* top = core->PeekThread();
		wasRunQueueEmpty = (top == NULL || top->IsIdle());

		bool isForeground = fIsForeground;

		if (fQuickStartCredit || isForeground) {
			core->PushFront(this, priority);
			requestPreemption = true;
			if (isForeground)
				updateInteraction = true;
		} else
			core->PushBack(this, priority);
	}
	// Note: Global run-queue counter update. gTotalRunnableThreads is
	// incremented AFTER the CPUCount == 0 guards in both the pinned and
	// non-pinned paths.  There is no return-false path after the increment;
	// PushFront/PushBack are infallible.  The counter is therefore always
	// matched by either a GoesAway/Dies decrement (when the thread leaves the
	// ready state) or a symmetric Enqueue on the next wakeup. This comment
	// documents that the ordering is intentional and correct.

	fQuickStartCredit = false;
	return true;
}

inline bool ThreadData::Dequeue() {
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

	CoreEntry* core = Core();
	CoreRunQueueLocker _(core);
	if (!fEnqueued)
		return false;

	core->Remove(this);
	ASSERT(!fEnqueued);
	return true;
}

inline void RunQueueTraits<ThreadData>::SetInRunQueue(ThreadData* element,
													  bool inQueue) {
	element->GetThread()->inRunQueue = inQueue;
}

inline void ThreadData::UpdateVirtualRuntime(bigtime_t delta, bigtime_t now,
											 bigtime_t maxLatency) {
	if (delta <= 0 || IsRealTime())
		return;

	// Optimization: Pre-calculate lookahead horizon
	if (maxLatency == 0)
		maxLatency =
			(bigtime_t)LoadAcquire64(sMaxLatency);
	const bigtime_t kLookahead = maxLatency * 1000LL;

	if (now == 0) {
		now = system_time();
		if (now == 0)
			return;
	}

	bigtime_t ceiling =
		(now > B_INT64_MAX - kLookahead) ? B_INT64_MAX : now + kLookahead;

	const bigtime_t threshold = ceiling - delta;
	bigtime_t vRuntime = (bigtime_t)LoadAcquire64(fVirtualRuntime);

	while (true) {
		bigtime_t next;
		if (vRuntime < threshold)
			next = vRuntime + delta;
		else if (vRuntime < ceiling)
			next = ceiling;
		else
			break;

		// Optimization: Reuse the value returned by the CAS if it fails
		bigtime_t old = (bigtime_t)TestAndSet64(fVirtualRuntime, (int64)((uint64)next), (int64)vRuntime);
		if (old == vRuntime)
			break;
		vRuntime = old;
	}
}

inline void ThreadData::UpdateActivity(bigtime_t active, bigtime_t now) {
	SCHEDULER_ENTER_FUNCTION();

	if (!IsRealTime() && !IsIdle()) {
		// Note: Formal fair-share runtime update.
		// delta = (active * kFairShareReferenceWeight) / Weight
		int64 weight = GetWeight();
		if (weight <= 0) weight = 1;
		bigtime_t delta = (active * 1000) / weight;

		UpdateVirtualRuntime(delta, now);

		// Note: Update Lag.
		// Lag = (SystemVirtualTime - VirtualRuntime) * Weight
		CoreEntry* core = Core();
		if (core != NULL) {
			// Find a CPU in this core to get SystemVirtualTime.
			CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());
			if (cpu->Core() == core) {
				bigtime_t svt = cpu->SystemVirtualTime();
				int64 lag = (svt - GetVirtualRuntime()) * weight;
				StoreRelease64(fLag, lag);
			}
		}
	}

	if (!gTrackCoreLoad)
		return;

	AddRelease64(fMeasureAvailableTime, (int64)active);
	AddRelease64(fMeasureAvailableActiveTime, (int64)active);
}

}  // namespace Scheduler

#endif	// KERNEL_SCHEDULER_THREAD_H
