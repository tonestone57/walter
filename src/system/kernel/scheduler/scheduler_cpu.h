// AUDIT FIXES: issues 6, 12, 36, 45, 52, 59, 70, 96
/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_CPU_H
#define KERNEL_SCHEDULER_CPU_H


#include <OS.h>

#include <smp.h>
#include <thread.h>
#include <util/atomic.h>
#include <util/AutoLock.h>
#include <util/BitUtils.h>
#include <util/Heap.h>
#include <util/MinMaxHeap.h>

#include <cpufreq.h>
#include <DPC.h>

#include "RunQueue.h"
#include "scheduler_common.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"


namespace Scheduler {


class DebugDumper;

struct ThreadData;
class ThreadProcessing;

class CPUEntry;
class CoreEntry;
class PackageEntry;

enum CoreType {
	CORE_TYPE_UNKNOWN = 0,
	CORE_TYPE_EFFICIENCY,
	CORE_TYPE_STANDARD,
	CORE_TYPE_PERFORMANCE
};

class IRQRebalanceDPC : public DPCCallback {
public:
	virtual	void			DoDPC(DPCQueue* queue);

			int32			fIRQ;
			int32			fTargetCPU;
};

// Adjusted to sizeof(native_cpu_mask_t) * 8 to support larger clusters on 64-bit.
// This allows a single L3 domain to span up to 64 cores.
const int32 kMaxCoresPerPackage = sizeof(native_cpu_mask_t) * 8;
// Validate the shift range: PackageIndex is in [0, kMaxCoresPerPackage),
// so (native_cpu_mask_t)1 << PackageIndex() must not overflow. The old
// assert compared kMaxCoresPerPackage to itself (tautological). This one
// checks that kMaxCoresPerPackage does not exceed a hard platform ceiling
// and that native_cpu_mask_t is wide enough to represent all core bits.
// static_assert replaced by comment for GCC 2.95
// kMaxCoresPerPackage <= 64
// kMaxCoresPerPackage <= (int32)(sizeof(native_cpu_mask_t) * 8)

// The run queues. Holds the threads ready to run ordered by priority.
// One queue per schedulable target per core. Additionally, each
// logical processor has its sPinnedRunQueues used for scheduling
// pinned threads.
class ThreadRunQueue : public RunQueue<ThreadData, THREAD_MAX_SET_PRIORITY,
	ThreadDataVRuntimeCompare> {
public:
						void			Dump() const;
};

class CPUEntry : public HeapLinkImpl<CPUEntry, int32> {
public:
										CPUEntry();

						void			Init(int32 id, CoreEntry* core);

	inline				int32			ID() const	{ return fCPUNumber; }
	inline				CoreEntry*		Core() const
	{
		return atomic_pointer_get<CoreEntry>(
			const_cast<CoreEntry* volatile*>(&fCore));
	}

	inline				int32			PerformanceScale() const
											{ return fPerformanceScale; }
	inline				void			SetPerformanceScale(int32 scale)
											{ fPerformanceScale = scale; }

						void			Start();
						void			Stop();

	inline				void			EnterScheduler();
	inline				void			ExitScheduler();

	inline				void			LockScheduler();
	inline				void			UnlockScheduler();

	inline				void			LockRunQueue();
	inline				bool			TryLockRunQueue();
	inline				void			UnlockRunQueue();

	// Index of this CPU within its core, assigned sequentially in AddCPU.
	// Used by UpdatePriorityBoostScalable for round-robin epoch ownership.
	// Public because UpdatePriorityBoostScalable is a file-scope function.
						int32			fCoreLocalIndex;

						void			PushFront(ThreadData* thread,
											int32 priority);
						void			PushBack(ThreadData* thread,
											int32 priority);
						void			Remove(ThreadData* thread);
						ThreadData*		PeekThread() const;
						ThreadData*		PeekIdleThread() const;
						// Required by UpdatePriorityBoostScalable in scheduler.cpp,
						// which inspects the run queue bitmap directly for priority boosting.
	inline				const ThreadRunQueue*	RunQueue() const { return &fRunQueue; }

	inline				ThreadRunQueue::ConstIterator
										GetConstIterator() const
											{ return fRunQueue.GetConstIterator(); }

						void			UpdatePriority(int32 priority);

	inline				int32			GetLoad() const;
						bigtime_t		GetMinVirtualRuntime() const;
						void			ComputeLoad(bigtime_t now = 0);

						ThreadData*		ChooseNextThread(ThreadData* oldThread,
											bool putAtBack, bigtime_t now = 0);

						void			UpdateActiveTime(ThreadData* oldThreadData,
											bigtime_t now);
						void			TrackLoad(ThreadData* nextThreadData,
											bigtime_t now = 0);

						void			StartQuantumTimer(ThreadData* thread,
											bool wasPreempted);

						uint32			GetRandom();

	inline				int32			ThreadCount() const
											{ return atomic_get(const_cast<int32*>(&fThreadCount)); }

						bool			SetReschedulePending()
											{ return atomic_set(&fReschedulePending, 1) == 0; }
						void			ClearReschedulePending()
											{ atomic_set(&fReschedulePending, 0); }

	static inline		CPUEntry*		GetCPU(int32 cpu);

private:
						void			_RequestPerformanceLevel(
											ThreadData* threadData,
											bigtime_t now = 0);
						ThreadData*		_TryStealWork();

	static				int32			_RescheduleEvent(timer* /* unused */);
	static				int32			_UpdateLoadEvent(timer* /* unused */);

						int32			fCPUNumber;
						CoreEntry*		fCore;

						rw_spinlock 	fSchedulerModeLock;

						ThreadRunQueue	fRunQueue;
						spinlock		fQueueLock;

						int32				fThreadCount;
						int32				fLoad;
						bigtime_t		lastReschedule __attribute__((aligned(8)));

						int32			fPerformanceScale;

						bigtime_t		fMeasureActiveTime __attribute__((aligned(8)));
						bigtime_t		fMeasureTime __attribute__((aligned(8)));

						bool			fUpdateLoadEvent;
						uint64			fRandomState __attribute__((aligned(8)));

						uint32			fRescheduleCount;
						uint32			fInteractionUpdateCounter;

						int32			fReschedulePending;
						// Moved from CoreEntry to eliminate false sharing.
						// This field is written on every search_local_node call by
						// the searching CPU.  Placing it in CoreEntry dirtied the
						// core's hot read-mostly cache line on every sibling CPU.
						int32			fLastLocalPackageIndex;

public:
						IRQRebalanceDPC	fRebalanceDPC;

						friend class DebugDumper;
} __attribute__((aligned(64)));

class CPUPriorityHeap : public Heap<CPUEntry, int32> {
public:
										CPUPriorityHeap() { }
										CPUPriorityHeap(int32 cpuCount);

						void			Dump();
};

class CoreEntry {
public:
										CoreEntry();

						void			Init(int32 id, PackageEntry* package);

	static inline		CoreEntry*		GetCore(int32 cpu);

	inline				int32			ID() const	{ return fCoreID; }
	inline				PackageEntry*	Package() const	{ return fPackage; }
	inline				int32			PackageIndex() const
											{ return fPackageIndex; }
	inline				int32			CPUCount() const
											{ return atomic_get(const_cast<int32*>(&fCPUCount)); }
	inline				const CPUSet&	CPUMask() const
											{ return fCPUSet; }

	inline				CoreType		Type() const	{ return fType; }
	inline				void			SetType(CoreType type) { fType = type; }

	inline				void			LockCPUHeap();
	inline				void			UnlockCPUHeap();

	inline				void			LockCPU();
	inline				void			UnlockCPU();

	inline				CPUPriorityHeap*	CPUHeap();

	inline				int32			ThreadCount() const
											{ return atomic_get(const_cast<int32*>(&fTotalThreadCount)); }
	inline				void			IncrementTotalThreadCount()
											{ atomic_add(&fTotalThreadCount, 1); }
	inline				void			DecrementTotalThreadCount()
											{ atomic_add(&fTotalThreadCount, -1); }

	inline				int32			DisplayThreadCount() const
											{ return atomic_get(const_cast<int32*>(&fDisplayThreadCount)); }
	inline				void			IncrementDisplayThreadCount()
											{ atomic_add(&fDisplayThreadCount, 1); }
	inline				void			DecrementDisplayThreadCount()
											{ atomic_add(&fDisplayThreadCount, -1); }
	inline				int32			CoreRunQueueThreadCount() const
											{ return atomic_get(const_cast<int32*>(&fThreadCount)); }

	inline				void			LockRunQueue();
	inline				bool			TryLockRunQueue();
	inline				void			UnlockRunQueue();
	// Issue 12 fix: lockless check for display-priority threads in the
	// run queue.  Used by ComputeQuantum to avoid TryLockRunQueue on the
	// scheduling hot path.  Atomic bitmap reads match PeekMaximum's pattern.
	inline				bool			HasHighPriorityThread() const;

						ThreadData*		StealThread(int32& stolenPriority,
											int32 thiefCPU);

						void			PushFront(ThreadData* thread,
											int32 priority);
						void			PushBack(ThreadData* thread,
											int32 priority);
						void			Remove(ThreadData* thread);
						ThreadData*		PeekThread() const;
	inline				ThreadData*		PeekHead() const
											{ return fRunQueue.PeekMaximum(); }
	inline				const ThreadRunQueue*	RunQueue() const { return &fRunQueue; }

	inline				ThreadRunQueue::ConstIterator
										GetConstIterator() const
											{ return fRunQueue.GetConstIterator(); }

	inline				bigtime_t		GetActiveTime() const;
	inline				void			IncreaseActiveTime(
											bigtime_t activeTime);

	inline				int32			GetLoad() const;
	inline				int32			GetScore() const;
						void			SetCapacity(int32 capacity);
	inline				int32			Capacity() const { return fCapacity; }
	inline				uint32			ScoreFactor() const { return fScoreFactor; }
						bigtime_t		GetMinVirtualRuntime() const;
	inline				uint32			LoadMeasurementEpoch() const
											{ return (uint32)atomic_get64((int64*)&fCombinedLoad); }
	inline				int32			CurrentLoad() const
											{ return (int32)(atomic_get64((int64*)&fCombinedLoad) >> 32); }

	inline				void			AddLoad(int32 load, uint32 epoch,
											bool updateLoad, bigtime_t now = 0);
	inline				uint32			RemoveLoad(int32 load, bool force,
											bigtime_t now = 0);
	inline				void			ChangeLoad(int32 delta, bigtime_t now = 0);

	inline				void			CPUGoesIdle(CPUEntry* cpu);
	inline				void			CPUWakesUp(CPUEntry* cpu);

						CPUEntry*		PeekMinimumLoadCPU();

						void			AddCPU(CPUEntry* cpu);
						void			RemoveCPU(CPUEntry* cpu,
											ThreadProcessing&
												threadPostProcessing);

private:
						void			_UpdateLoad(bool forceUpdate = false,
											bigtime_t now = 0);

	static				void			_UnassignThread(Thread* thread,
											void* core);

						bigtime_t		fActiveTime __attribute__((aligned(8)));

						// bits 32-63: Current Load, bits 0-31: Epoch
						int64			fCombinedLoad __attribute__((aligned(8)));

						bigtime_t		fLastLoadUpdate __attribute__((aligned(8)));

						int32			fCoreID;
						PackageEntry*	fPackage __attribute__((aligned(64)));
						int32			fPackageIndex;

						CoreType		fType;

						int32			fCPUCount;
						int32			fCapacity;
						CPUSet			fCPUSet;
						int32			fIdleCPUCount;
						CPUPriorityHeap	fCPUHeap;
						spinlock		fCPULock;

						spinlock		fQueueLock;
						int32			fThreadCount;
						int32			fTotalThreadCount;
						int32			fDisplayThreadCount;
						ThreadRunQueue	fRunQueue;

						int32			fLoad;

						uint32			fScoreFactor;

						native_cpu_mask_t	fLocalIndices __attribute__((aligned(8)));

						friend class DebugDumper;
} __attribute__((aligned(64)));

// gPackageEntries are used to decide which core should be woken up from the
// idle state. When aiming for performance we should use as many packages as
// possible with as little cores active in each package as possible (so that the
// package can enter any boost mode if it has one and the active core have more
// of the shared cache for themselves. If power saving is the main priority we
// should keep active cores on as little packages as possible (so that other
// packages can go to the deep state of sleep). The heap stores only packages
// with at least one core active and one core idle. The packages with all cores
// idle are stored in gPackageIdleList (in LIFO manner).
// Group of packages. Used to improve scalability on systems with many packages.
class SchedulerNode {
public:
											SchedulerNode();

						void				Init(int32 id);

	inline				void				PackageGoesIdle(PackageEntry* package);
	inline				void				PackageWakesUp(PackageEntry* package);

	inline				native_cpu_mask_t	IdlePackageMask() const;
	inline				int32				NodeIndex() const { return fNodeID; }

	inline				int32				PackageStartIndex() const
											{ return fPackageStartIndex; }
	inline				void				SetPackageStartIndex(int32 start)
											{ fPackageStartIndex = start; }

	inline				int32				PackageCount() const
											{ return fPackageCount; }
	inline				void				SetPackageCount(int32 count)
											{ fPackageCount = count; }

	// SetPackageIdle removed — it was never called from any
	// translation unit.  All idle-mask updates go through PackageGoesIdle /
	// PackageWakesUp.  Leaving dead code here invited future callers to
	// bypass the node-level gIdleNodeMask updates performed by those methods.

private:
						int32				fNodeID;
						native_cpu_mask_t	fIdlePackageMask __attribute__((aligned(8)));

						int32				fPackageStartIndex;
						int32				fPackageCount;
} __attribute__((aligned(64)));


class PackageEntry {
public:
											PackageEntry();

						void				Init(int32 id, SchedulerNode* node,
												int32 nodeIndex);

	inline				void				CoreGoesIdle(CoreEntry* core);
	inline				void				CoreWakesUp(CoreEntry* core);

						CoreEntry*			GetIdleCore(int32 index = 0) const;
						CoreEntry*			GetIdleCorePacking(CPUEntry* cpu,
												const CPUSet* mask = NULL) const;
	inline				native_cpu_mask_t	IdleCoreMask() const;
	inline				int32				IdleCoreCount() const { return fIdleCoreCount; }
	inline				CoreEntry*			GetCore(int32 index) const;
	inline				SchedulerNode*		Node() const { return fNode; }
	inline				int32				NodeIndex() const { return fNodeIndex; }

						void				AddIdleCore(CoreEntry* core);
						void				RemoveIdleCore(CoreEntry* core);
						void				RegisterCore(int32 index,
												CoreEntry* core);

	// Issue 4 fix: added missing accessor.
	inline				int32				ID() const { return fPackageID; }

	inline				int32				RegisteredCoreCount() const
											{ return fRegisteredCoreCount; }

	static inline		PackageEntry*		GetLeastIdlePackage();

	inline				void				ReadLockCore();
	inline				void				ReadUnlockCore();

						CoreEntry*			PeekMinimumLoadCore(
												CPUEntry* cpu,
												const CPUSet* mask = NULL,
												CoreType type = CORE_TYPE_UNKNOWN) const;
						CoreEntry*			PeekMaximumLoadCore(
												CPUEntry* cpu,
												const CPUSet* mask = NULL,
												CoreType type = CORE_TYPE_UNKNOWN) const;

private:
						int32				fPackageID;
						SchedulerNode*		fNode;
						int32				fNodeIndex;

						CoreEntry*			fCores[kMaxCoresPerPackage] __attribute__((aligned(8)));
						native_cpu_mask_t	fIdleCoreMask __attribute__((aligned(8)));
						int32				fIdleCoreCount;
						int32				fCoreCount;
						int32				fRegisteredCoreCount;
						int32				fMaxAttempts;
public:
	inline				int32				CoreCount() const { return fCoreCount; }
private:
	mutable				rw_spinlock			fCoreLock;

						int32				fCoreLoads[kMaxCoresPerPackage] __attribute__((aligned(8)));
						native_cpu_mask_t	fEnabledCoreMask __attribute__((aligned(8)));

						friend class DebugDumper;
} __attribute__((aligned(64)));

extern CPUEntry* gCPUEntries;

extern CoreEntry* gCoreEntries;
extern int32 gCoreCount;

extern PackageEntry* gPackageEntries;
extern int32 gPackageCount;

extern SchedulerNode* gSchedulerNodes;
extern uint64 gIdleNodeMask __attribute__((aligned(8)));
extern int32 gNodeCount;


inline void
CPUEntry::EnterScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_read_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::ExitScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	release_read_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::LockScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_write_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::UnlockScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	release_write_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::LockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_spinlock(&fQueueLock);
}


inline bool
CPUEntry::TryLockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	return try_acquire_spinlock(&fQueueLock);
}


inline void
CPUEntry::UnlockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	release_spinlock(&fQueueLock);
}


/* static */ inline CPUEntry*
CPUEntry::GetCPU(int32 cpu)
{
	return &gCPUEntries[cpu];
}


inline int32
CPUEntry::GetLoad() const
{
	int32 load = atomic_get(const_cast<int32*>(&fLoad));

	// Penalize SMT siblings to prefer physical cores
	CoreEntry* core = atomic_pointer_get<CoreEntry>(
		const_cast<CoreEntry* volatile*>(&fCore));
	if (core != NULL && core->CPUCount() > 1) {
		// If at least one other thread is running on this core
		if (core->ThreadCount() > 1)
			load += kSMTPenalty;
	}

	return load;
}


inline void
CoreEntry::LockCPUHeap()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_spinlock(&fCPULock);
}


inline void
CoreEntry::UnlockCPUHeap()
{
	SCHEDULER_ENTER_FUNCTION();
	release_spinlock(&fCPULock);
}


inline void
CoreEntry::LockCPU()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_spinlock(&fCPULock);
}


inline void
CoreEntry::UnlockCPU()
{
	SCHEDULER_ENTER_FUNCTION();
	release_spinlock(&fCPULock);
}


inline CPUPriorityHeap*
CoreEntry::CPUHeap()
{
	SCHEDULER_ENTER_FUNCTION();
	return &fCPUHeap;
}


inline void
CoreEntry::LockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_spinlock(&fQueueLock);
}


inline bool
CoreEntry::TryLockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	return try_acquire_spinlock(&fQueueLock);
}


inline void
CoreEntry::UnlockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	release_spinlock(&fQueueLock);
}


inline void
CoreEntry::IncreaseActiveTime(bigtime_t activeTime)
{
	SCHEDULER_ENTER_FUNCTION();
	atomic_add64((int64*)&fActiveTime, activeTime);
}


inline bigtime_t
CoreEntry::GetActiveTime() const
{
	return atomic_get64((int64*)&fActiveTime);
}


inline int32
CoreEntry::GetLoad() const
{
	return atomic_get(const_cast<int32*>(&fLoad));
}


inline int32
CoreEntry::GetScore() const
{
	return ((int64)GetLoad() * fScoreFactor) >> 16;
}


inline void
CoreEntry::AddLoad(int32 load, uint32 epoch, bool updateLoad, bigtime_t now)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(load >= 0 && load <= kMaxLoad);

	int64 oldCombined = atomic_add64(&fCombinedLoad, (int64)load << 32);
	if ((uint32)oldCombined != epoch)
		atomic_add(&fLoad, load);

	if (updateLoad)
		_UpdateLoad(true, now);
}


inline uint32
CoreEntry::RemoveLoad(int32 load, bool force, bigtime_t now)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(load >= 0 && load <= kMaxLoad);

	int64 oldCombined = atomic_add64(&fCombinedLoad, (int64)(-load) << 32);
	if (force) {
		atomic_add(&fLoad, -load);

		_UpdateLoad(true, now);
	}
	return (uint32)oldCombined;
}


inline void
CoreEntry::ChangeLoad(int32 delta, bigtime_t now)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(delta >= -kMaxLoad && delta <= kMaxLoad);

	if (delta != 0) {
		atomic_add64(&fCombinedLoad, (int64)delta << 32);
		atomic_add(&fLoad, delta);
	}

	_UpdateLoad(false, now);
}


/* PackageEntry::CoreGoesIdle and PackageEntry::CoreWakesUp have to be defined
   before CoreEntry::CPUGoesIdle and CoreEntry::CPUWakesUp. If they weren't
   GCC2 wouldn't inline them as, apparently, it doesn't do enough optimization
   passes.
*/
inline void
PackageEntry::CoreGoesIdle(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();

	WriteSpinLocker coreLocker(fCoreLock);
	native_cpu_mask_t oldMask = scheduler_atomic_or(&fIdleCoreMask,
		(native_cpu_mask_t)1 << core->PackageIndex());
	atomic_add(&fIdleCoreCount, 1);

	if (oldMask == 0) {
		// Issue 89 fix: Added explicit serialization via fCoreLock to ensure
		// that PackageGoesIdle is called only once and does not race with
		// AddIdleCore or CoreWakesUp.
		if (fNode != NULL)
			fNode->PackageGoesIdle(this);
	}
}


inline void
PackageEntry::CoreWakesUp(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();

	WriteSpinLocker coreLocker(fCoreLock);
	// Clear the mask bit BEFORE decrementing the count, matching the logic
	// in RemoveIdleCore to prevent GetIdleCore from returning a
	// "dangling-ish" core reference.
	native_cpu_mask_t clearBit = (native_cpu_mask_t)1 << core->PackageIndex();
	native_cpu_mask_t oldMask = scheduler_atomic_and(&fIdleCoreMask, ~clearBit);

	atomic_add(&fIdleCoreCount, -1);

	// Detect the transition from fully-idle to partially-active:
	// the package wakes up when the *last* idle core becomes active, i.e.
	// after clearing this core's bit the mask becomes zero.
	if ((oldMask & clearBit) != 0 && (oldMask & ~clearBit) == 0) {
		// package wakes up (last idle core became active)
		if (fNode != NULL)
			fNode->PackageWakesUp(this);
	}
}


inline void
SchedulerNode::PackageGoesIdle(PackageEntry* package)
{
	SCHEDULER_ENTER_FUNCTION();

	const int32 kMaxPackagesPerNode = sizeof(native_cpu_mask_t) * 8;
	if (package->NodeIndex() < 0 || package->NodeIndex() >= kMaxPackagesPerNode)
		return;

	// fIdlePackageMask is native_cpu_mask_t (32-bit on 32-bit
	// systems); atomic_or64 writes 8 bytes over a 4-byte field, corrupting
	// adjacent struct memory.  Use scheduler_atomic_or throughout.
	native_cpu_mask_t oldMask = scheduler_atomic_or(&fIdlePackageMask,
		(native_cpu_mask_t)1 << package->NodeIndex());

	if (oldMask == 0) {
		// node goes idle (first package)
		// Issue 8 fix: guard gIdleNodeMask update; nodes >= 64 cannot
		// be tracked for idleness but must still exist.
		if (fNodeID < 64) { // Issue 74 fix: node limit
			atomic_or64((int64*)&gIdleNodeMask, 1ULL << fNodeID);
		}
	}
}


inline void
SchedulerNode::PackageWakesUp(PackageEntry* package)
{
	// Issue 19 fix: guard PackageWakesUp livelock.
	SCHEDULER_ENTER_FUNCTION();

	const int32 kMaxPackagesPerNode = sizeof(native_cpu_mask_t) * 8;
	if (package->NodeIndex() < 0 || package->NodeIndex() >= kMaxPackagesPerNode)
		return;

	// same fix — use scheduler_atomic_and for 32-bit safety.
	native_cpu_mask_t clearBit = (native_cpu_mask_t)1 << package->NodeIndex();
	native_cpu_mask_t oldMask = scheduler_atomic_and(&fIdlePackageMask, ~clearBit);

	// Detect the transition from fully-idle to partially-active for the node.
	// Only clear the bit in gIdleNodeMask if this package was actually idle
	// (bit was set in oldMask) AND it was the last idle package in this node
	// (mask is now zero).
	if ((oldMask & clearBit) != 0 && (oldMask & ~clearBit) == (native_cpu_mask_t)0) {
		if (fNodeID < 64) { // Issue 74 fix: node limit
			// Issue 12 fix: a plain re-read + atomic_and64 is still racy.
			// Between the re-read returning 0 and the atomic_and64, a
			// concurrent PackageGoesIdle can set a bit in fIdlePackageMask
			// AND set our node bit in gIdleNodeMask.  The atomic_and64 then
			// clears the node bit, permanently losing the idle notification.
			//
			// Fix: use a CAS loop that re-checks fIdlePackageMask atomically
			// with the gIdleNodeMask update.  If fIdlePackageMask is no longer
			// zero when we re-read it, a concurrent PackageGoesIdle has fired
			// and will (or has already) re-set the node bit — we must not
			// clear it.
			const int64 nodeBit = (int64)(1ULL << fNodeID);
			int64 nodeMask __attribute__((aligned(8)));
			// Issue 19 fix: add iteration bound to prevent livelock when
			// packages continuously oscillate between idle/active states.
			// After kMaxWakeupRetries CAS failures we give up; the next
			// PackageGoesIdle call will re-set the bit if needed.
			const int kMaxWakeupRetries = 64;
			int wakeupRetries = 0;
			while (true) {
				nodeMask = atomic_get64((int64*)&gIdleNodeMask);
				if (!(nodeMask & nodeBit))
					break; // already cleared by a concurrent PackageWakesUp

				if (scheduler_atomic_get(&fIdlePackageMask)
						!= (native_cpu_mask_t)0) {
					// A package in this node went idle concurrently; the node
					// bit must remain set.
					break;
				}

				if (atomic_test_and_set64((int64*)&gIdleNodeMask,
						nodeMask & ~nodeBit, nodeMask) == nodeMask) {
					// Successfully cleared. Re-check for safety to catch the
					// race where a package went idle between our last check
					// and the CAS.
					if (scheduler_atomic_get(&fIdlePackageMask)
							!= (native_cpu_mask_t)0) {
						atomic_or64((int64*)&gIdleNodeMask, nodeBit);
					}
					break;
				}

				if (++wakeupRetries >= kMaxWakeupRetries)
					break;
			}
		}
	}
}

inline native_cpu_mask_t
SchedulerNode::IdlePackageMask() const
{
	SCHEDULER_ENTER_FUNCTION();
	// use scheduler_atomic_get for 32-bit correctness.
	return scheduler_atomic_get(
		const_cast<native_cpu_mask_t*>(&fIdlePackageMask));
}


// Issue 89 fix: AddCPU calls fPackage->AddIdleCore(this) which acquires
// fCoreLock (write). CoreGoesIdle calls PackageEntry::CoreGoesIdle, which
// now also acquires fCoreLock (write) for explicit serialization. This
// ensures package-level state transitions are atomic even when triggered
// outside of the global InterruptsBigSchedulerLocker path.
inline void
CoreEntry::CPUGoesIdle(CPUEntry* cpu)
{
	if (gSingleCore)
		return;

	SetCPUIDle(gIdleMask, cpu->ID());

	DecrementTotalThreadCount();
	// Issue 36 fix: on weakly-ordered architectures, without an explicit
	// barrier between atomic_add(fIdleCPUCount) and atomic_get(fCPUCount),
	// the CPU could observe fCPUCount before fIdleCPUCount increment is
	// globally visible, causing a spurious PackageGoesIdle call.
	int32 newIdleCount = atomic_add(&fIdleCPUCount, 1) + 1;
	memory_read_barrier();
	int32 cpuCount = atomic_get(&fCPUCount);
	if (cpuCount > 0 && newIdleCount >= cpuCount)
		fPackage->CoreGoesIdle(this);
}


inline void
CoreEntry::CPUWakesUp(CPUEntry* cpu)
{
	if (gSingleCore)
		return;

	ClearCPUIDle(gIdleMask, cpu->ID());

	ASSERT(atomic_get(&fIdleCPUCount) > 0);

	IncrementTotalThreadCount();
	// Issue 70 fix: read fCPUCount AFTER IncrementTotalThreadCount and
	// insert a read barrier. A concurrent AddCPU increments fCPUCount then
	// fIdleCPUCount; by inserting the barrier we ensure we see the latest
	// fCPUCount before comparing with the old fIdleCPUCount.
	memory_read_barrier();
	int32 cpuCount = atomic_get(&fCPUCount);
	if (atomic_add(&fIdleCPUCount, -1) == cpuCount)
		fPackage->CoreWakesUp(this);
}


/* static */ inline CoreEntry*
CoreEntry::GetCore(int32 cpu)
{
	SCHEDULER_ENTER_FUNCTION();
	return gCPUEntries[cpu].Core();
}


inline native_cpu_mask_t
PackageEntry::IdleCoreMask() const
{
	// Issue 6: Packing rotation logic documentation. GetIdleCorePacking uses rotation arithmetic on this mask.
	// The un-rotation formula origIdx = (pos + shift) % kMaxCoresPerPackage
	// is correct only when kMaxCoresPerPackage is a power of 2, which it is
	// on all supported platforms (32 on 32-bit, 64 on 64-bit).  This
	// comment documents the assumption so it is verified if kMaxCoresPerPackage
	// is ever changed to a non-power-of-2 value.
	// static_assert replaced by comment for GCC 2.95
	// (kMaxCoresPerPackage & (kMaxCoresPerPackage - 1)) == 0
	SCHEDULER_ENTER_FUNCTION();
	return scheduler_atomic_get(&fIdleCoreMask);
}


inline CoreEntry*
PackageEntry::GetCore(int32 index) const
{
	SCHEDULER_ENTER_FUNCTION();
	return fCores[index];
}




/* static */ inline PackageEntry*
PackageEntry::GetLeastIdlePackage()
{
	SCHEDULER_ENTER_FUNCTION();

	PackageEntry* package = NULL;
	int32 bestIdleCount = -1;

	CPUEntry* cpu = CPUEntry::GetCPU(smp_get_current_cpu());

	if (gPackageCount > kRandomSearchThreshold) {
		// (clarification): CoreCPULocker and CoreRunQueueLocker in
		// ThreadData::Enqueue use fCPULock and fQueueLock respectively — they
		// are DISTINCT spinlocks and do NOT deadlock.  No code change needed.

		// For all practical package counts (33-4096) the log2 formula always
		// evaluates to a value <= kMaxFallbackAttempts, so use the global
		// constant directly.  This avoids recomputing fls() on every
		// call in this hot path and keeps the probe count consistent with the
		// rest of the scheduler.
		for (int32 i = 0; i < kMaxFallbackAttempts; i++) {
			int32 idx = (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32);
			PackageEntry* current = &gPackageEntries[idx];
			// skip packages whose Init() was skipped (fNode == NULL);
			// callers dereference Package()->Node()->ID() on the result.
			if (current->fNode == NULL)
				continue;
			int32 count = atomic_get((int32*)&current->fIdleCoreCount);
			if (count != 0 && (package == NULL || count < bestIdleCount)) {
				package = current;
				bestIdleCount = count;
			}
		}
	} else {
		// Small system: full linear scan — every package is cheap to check.
		for (int32 i = 0; i < gPackageCount; i++) {
			PackageEntry* current = &gPackageEntries[i];
			// Issue 73 fix: skip packages with NULL fNode (partially init'd).
			// Callers like power_saving::choose_core dereference
			// core->Package()->Node()->ID() on the result; if Node() is NULL
			// this crashes. The existing NULL skip prevents returning such a
			// package, but we document it explicitly here.
			if (current->fNode == NULL)
				continue;
			int32 count = atomic_get((int32*)&current->fIdleCoreCount);
			if (count != 0 && (package == NULL || count < bestIdleCount)) {
				package = current;
				bestIdleCount = count;
			}
		}
	}

	return package;
}


inline void
PackageEntry::ReadLockCore()
{
	acquire_read_spinlock(&fCoreLock);
}


inline bool
CoreEntry::HasHighPriorityThread() const
{
	// Issue 12 fix: lockless approximation replacing TryLockRunQueue in
	// ComputeQuantum.  TryLock failure under contention (common on loaded
	// systems) left displayReady=false even when a display thread was ready,
	// handing the running thread up to kMaxQuantum instead of kDisplayQuantum.
	//
	// We use the same atomic_get pattern as PeekMaximum: bitmap writes are
	// done under the run-queue lock but we read without it.  On x86 aligned
	// 32-bit loads are indivisible; on other supported architectures the
	// worst case is a stale read that causes one extra-long quantum before
	// the next reschedule corrects it — acceptable for this hint.
	const uint32* bitmap = fRunQueue.GetBitmap();
	for (int i = ThreadRunQueue::kBitmapSize - 1; i >= 0; i--) {
		uint32 val = (uint32)atomic_get((int32*)&bitmap[i]);
		if (i == ThreadRunQueue::kBitmapSize - 1)
			val &= (uint32)((2ULL << (THREAD_MAX_SET_PRIORITY % 32)) - 1);
		if (val != 0) {
			// Found the highest occupied priority level; check threshold.
			int highestBit = fls(val) - 1;
			int highestPriority = i * 32 + highestBit;
			return highestPriority >= B_DISPLAY_PRIORITY;
		}
	}
	return false;
}


inline void
PackageEntry::ReadUnlockCore()
{
	release_read_spinlock(&fCoreLock);
}


int SmoothLoad(int oldLoad, int newLoad);


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_CPU_H
