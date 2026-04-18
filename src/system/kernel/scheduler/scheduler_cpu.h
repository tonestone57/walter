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
static_assert(kMaxCoresPerPackage <= 64,
	"kMaxCoresPerPackage exceeds 64-bit shift range on any platform");
static_assert(kMaxCoresPerPackage <= (int32)(sizeof(native_cpu_mask_t) * 8),
	"native_cpu_mask_t is too narrow for kMaxCoresPerPackage; "
	"increase native_cpu_mask_t or reduce kMaxCoresPerPackage");

// The run queues. Holds the threads ready to run ordered by priority.
// One queue per schedulable target per core. Additionally, each
// logical processor has its sPinnedRunQueues used for scheduling
// pinned threads.
class ThreadRunQueue : public RunQueue<ThreadData, THREAD_MAX_SET_PRIORITY,
	ThreadDataVRuntimeCompare> {
public:
						void			Dump() const;
};

class alignas(64) CPUEntry : public HeapLinkImpl<CPUEntry, int32> {
public:
										CPUEntry();

						void			Init(int32 id, CoreEntry* core);

	inline				int32			ID() const	{ return fCPUNumber; }
	inline				CoreEntry*		Core() const	{ return fCore; }

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

	inline				int32			GetLoad() const	{ return fLoad; }
						bigtime_t		GetMinVirtualRuntime() const;
						void			ComputeLoad();

						ThreadData*		ChooseNextThread(ThreadData* oldThread,
											bool putAtBack);

						void			UpdateActiveTime(ThreadData* oldThreadData);
						void			TrackLoad(ThreadData* nextThreadData);

						void			StartQuantumTimer(ThreadData* thread,
											bool wasPreempted);

						uint32			GetRandom();

	inline				int32			ThreadCount() const
											{ return atomic_get((int32*)&fThreadCount); }

						bool			SetReschedulePending()
											{ return atomic_set(&fReschedulePending, 1) == 0; }
						void			ClearReschedulePending()
											{ atomic_set(&fReschedulePending, 0); }

	static inline		CPUEntry*		GetCPU(int32 cpu);

private:
						void			_RequestPerformanceLevel(
											ThreadData* threadData);
						ThreadData*		_TryStealWork();

	static				int32			_RescheduleEvent(timer* /* unused */);
	static				int32			_UpdateLoadEvent(timer* /* unused */);

						int32			fCPUNumber;
						CoreEntry*		fCore;

						rw_spinlock 	fSchedulerModeLock;

						ThreadRunQueue	fRunQueue;
						spinlock		fQueueLock;

						int32			fThreadCount;
						int32			fLoad;

						int32			fPerformanceScale;

						bigtime_t		fMeasureActiveTime;
						bigtime_t		fMeasureTime;

						bool			fUpdateLoadEvent;
						uint64			fRandomState;

						uint32			fRescheduleCount;
						uint32			fInteractionUpdateCounter;

						int32			fReschedulePending;
						// Fix #14: Moved from CoreEntry to eliminate false sharing.
						// This field is written on every search_local_node call by
						// the searching CPU.  Placing it in CoreEntry dirtied the
						// core's hot read-mostly cache line on every sibling CPU.
						int32			fLastLocalPackageIndex;

public:
						IRQRebalanceDPC	fRebalanceDPC;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;

class CPUPriorityHeap : public Heap<CPUEntry, int32> {
public:
										CPUPriorityHeap() { }
										CPUPriorityHeap(int32 cpuCount);

						void			Dump();
};

class alignas(64) CoreEntry {
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
	inline				int32			CoreRunQueueThreadCount() const
											{ return atomic_get(const_cast<int32*>(&fThreadCount)); }

	inline				void			LockRunQueue();
	inline				bool			TryLockRunQueue();
	inline				void			UnlockRunQueue();

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
											bool updateLoad);
	inline				uint32			RemoveLoad(int32 load, bool force);
	inline				void			ChangeLoad(int32 delta);

	inline				void			CPUGoesIdle(CPUEntry* cpu);
	inline				void			CPUWakesUp(CPUEntry* cpu);

						CPUEntry*		PeekMinimumLoadCPU();

						void			AddCPU(CPUEntry* cpu);
						void			RemoveCPU(CPUEntry* cpu,
											ThreadProcessing&
												threadPostProcessing);

private:
						void			_UpdateLoad(bool forceUpdate = false);

	static				void			_UnassignThread(Thread* thread,
											void* core);

						bigtime_t		fActiveTime;

						// bits 32-63: Current Load, bits 0-31: Epoch
						int64			fCombinedLoad;

						bigtime_t		fLastLoadUpdate;

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
						ThreadRunQueue	fRunQueue;

						int32			fLoad;

						uint32			fScoreFactor;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;

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

	// Fix #13: SetPackageIdle removed — it was never called from any
	// translation unit.  All idle-mask updates go through PackageGoesIdle /
	// PackageWakesUp.  Leaving dead code here invited future callers to
	// bypass the node-level gIdleNodeMask updates performed by those methods.

private:
						int32				fNodeID;
						native_cpu_mask_t	fIdlePackageMask;

						int32				fPackageStartIndex;
						int32				fPackageCount;
} CACHE_LINE_ALIGN;


class PackageEntry {
public:
											PackageEntry();

						void				Init(int32 id, SchedulerNode* node,
												int32 nodeIndex);

	inline				void				CoreGoesIdle(CoreEntry* core);
	inline				void				CoreWakesUp(CoreEntry* core);

						CoreEntry*			GetIdleCore(int32 index = 0) const;
						CoreEntry*			GetIdleCorePacking(CPUEntry* cpu) const;
	inline				native_cpu_mask_t	IdleCoreMask() const;
	inline				int32				IdleCoreCount() const { return fIdleCoreCount; }
	inline				CoreEntry*			GetCore(int32 index) const;
	inline				SchedulerNode*		Node() const { return fNode; }
	inline				int32				NodeIndex() const { return fNodeIndex; }

						void				AddIdleCore(CoreEntry* core);
						void				RemoveIdleCore(CoreEntry* core);
						void				RegisterCore(int32 index,
												CoreEntry* core);

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

						CoreEntry*			fCores[kMaxCoresPerPackage];
						native_cpu_mask_t	fIdleCoreMask;
						int32				fIdleCoreCount;
						int32				fCoreCount;
						int32				fRegisteredCoreCount;
public:
	inline				int32				CoreCount() const { return fCoreCount; }
private:
	mutable				rw_spinlock			fCoreLock;

						int32				fCoreLoads[kMaxCoresPerPackage];
						native_cpu_mask_t	fEnabledCoreMask;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;

extern CPUEntry* gCPUEntries;

extern CoreEntry* gCoreEntries;
extern int32 gCoreCount;

extern PackageEntry* gPackageEntries;
extern int32 gPackageCount;

extern SchedulerNode* gSchedulerNodes;
extern uint64 gIdleNodeMask;
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
	SCHEDULER_ENTER_FUNCTION();
	return &gCPUEntries[cpu];
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
	SCHEDULER_ENTER_FUNCTION();
	return atomic_get64((int64*)&fActiveTime);
}


inline int32
CoreEntry::GetLoad() const
{
	SCHEDULER_ENTER_FUNCTION();

	// Read fLoad before fCPUCount: exact consistency is not required
	// (approximation is intentional on this hot path), and acquiring
	// the more-volatile counter first gives a marginally safer ordering.
	int32 load = atomic_get(const_cast<int32*>(&fLoad));
	int32 cpuCount = atomic_get(const_cast<int32*>(&fCPUCount));

	if (cpuCount <= 0)
		return kMaxLoad;

	// Optimization: Avoid division in the common cases.
	if (cpuCount == 1)
		return min_c(load, kMaxLoad);
	if (cpuCount == 2)
		return (int32)min_c(load >> 1, kMaxLoad);

	return (int32)min_c(load / cpuCount, kMaxLoad);
}


inline int32
CoreEntry::GetScore() const
{
	SCHEDULER_ENTER_FUNCTION();

	int32 load = GetLoad();

	// Use weighted score: (load * 1024) / capacity
	// This makes Efficiency cores (lower capacity) appear "full" faster.
	// Optimization: replaced division with multiplicative factor (fixed point 1.16)
	int64 score = ((int64)load * fScoreFactor) >> 16;
	return (int32)min_c(score, (int64)kMaxLoad);
}


inline void
CoreEntry::AddLoad(int32 load, uint32 epoch, bool updateLoad)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(load >= 0 && load <= kMaxLoad);

	int64 oldCombined = atomic_add64(&fCombinedLoad, (int64)load << 32);
	if ((uint32)oldCombined != epoch)
		atomic_add(&fLoad, load);

	if (updateLoad)
		_UpdateLoad(true);
}


inline uint32
CoreEntry::RemoveLoad(int32 load, bool force)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(load >= 0 && load <= kMaxLoad);

	int64 oldCombined = atomic_add64(&fCombinedLoad, (int64)(-load) << 32);
	if (force) {
		atomic_add(&fLoad, -load);

		_UpdateLoad(true);
	}
	return (uint32)oldCombined;
}


inline void
CoreEntry::ChangeLoad(int32 delta)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(delta >= -kMaxLoad && delta <= kMaxLoad);

	if (delta != 0) {
		atomic_add64(&fCombinedLoad, (int64)delta << 32);
		atomic_add(&fLoad, delta);
	}

	_UpdateLoad();
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

	native_cpu_mask_t oldMask = scheduler_atomic_or(&fIdleCoreMask,
		(native_cpu_mask_t)1 << core->PackageIndex());
	atomic_add(&fIdleCoreCount, 1);

	if (oldMask == 0) {
		// package goes idle (first core)
		if (fNode != NULL)
			fNode->PackageGoesIdle(this);
	}
}


inline void
PackageEntry::CoreWakesUp(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();

	// Clear the mask bit BEFORE decrementing the count, matching the logic
	// in RemoveIdleCore to prevent GetIdleCore from returning a
	// "dangling-ish" core reference.
	native_cpu_mask_t clearBit = (native_cpu_mask_t)1 << core->PackageIndex();
	native_cpu_mask_t oldMask = scheduler_atomic_and(&fIdleCoreMask, ~clearBit);

	atomic_add(&fIdleCoreCount, -1);

	// Detect the transition from fully-idle to partially-active:
	// the package wakes up when the *last* idle core becomes active, i.e.
	// after clearing this core's bit the mask becomes zero.
	if ((oldMask & ~clearBit) == 0) {
		// package wakes up (last idle core becomes active)
		if (fNode != NULL)
			fNode->PackageWakesUp(this);
	}
}


inline void
SchedulerNode::PackageGoesIdle(PackageEntry* package)
{
	SCHEDULER_ENTER_FUNCTION();

	if (package->NodeIndex() < 0 || package->NodeIndex() >= 64)
		return;

	uint64 oldMask = atomic_or64((int64*)&fIdlePackageMask, 1ULL << package->NodeIndex());

	if (oldMask == 0) {
		// node goes idle (first package)
		atomic_or64((int64*)&gIdleNodeMask, 1ULL << fNodeID);
	}
}


inline void
SchedulerNode::PackageWakesUp(PackageEntry* package)
{
	SCHEDULER_ENTER_FUNCTION();

	if (package->NodeIndex() < 0 || package->NodeIndex() >= 64)
		return;

	uint64 clearBit = 1ULL << package->NodeIndex();
	uint64 oldMask = (uint64)atomic_and64((int64*)&fIdlePackageMask, ~clearBit);

	// Detect the transition from fully-idle to partially-active for the node.
	// Only clear the bit in gIdleNodeMask if this package was actually idle
	// (bit was set in oldMask) AND it was the last idle package in this node
	// (mask is now zero).
	if ((oldMask & clearBit) != 0 && (oldMask & ~clearBit) == 0)
		atomic_and64((int64*)&gIdleNodeMask, ~(1ULL << fNodeID));
}


inline uint64
SchedulerNode::IdlePackageMask() const
{
	SCHEDULER_ENTER_FUNCTION();
	return atomic_get64((int64*)&fIdlePackageMask);
}


inline void
CoreEntry::CPUGoesIdle(CPUEntry* /* cpu */)
{
	if (gSingleCore)
		return;

	ASSERT(atomic_get(&fIdleCPUCount) < atomic_get(&fCPUCount));
	atomic_add(&fTotalThreadCount, -1);
	if (atomic_add(&fIdleCPUCount, 1) == atomic_get(&fCPUCount) - 1)
		fPackage->CoreGoesIdle(this);
}


inline void
CoreEntry::CPUWakesUp(CPUEntry* /* cpu */)
{
	if (gSingleCore)
		return;

	ASSERT(atomic_get(&fIdleCPUCount) > 0);

	atomic_add(&fTotalThreadCount, 1);
	// Fix #1 (documentation): == cpuCount is intentionally correct here.
	// atomic_add() returns the OLD value of fIdleCPUCount.  CoreWakesUp must
	// fire exactly on the transition "fully idle -> first CPU active", which
	// is when old_count == cpuCount (all CPUs were idle).  Changing this to
	// == 1 would fire when the *last* idle CPU in an already-active core
	// starts running — the opposite edge, and the wrong one for CoreWakesUp.
	// The symmetric CPUGoesIdle check (== cpuCount - 1) confirms the pattern:
	// both conditions detect the all-idle boundary from their respective sides.
	if (atomic_add(&fIdleCPUCount, -1) >= atomic_get(&fCPUCount))
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
		// For all practical package counts (33-4096) the log2 formula always
		// evaluates to a value <= kMaxFallbackAttempts, so use the global
		// constant directly.  This avoids recomputing __builtin_clz on every
		// call in this hot path and keeps the probe count consistent with the
		// rest of the scheduler.
		for (int32 i = 0; i < kMaxFallbackAttempts; i++) {
			int32 idx = (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32);
			PackageEntry* current = &gPackageEntries[idx];
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


inline void
PackageEntry::ReadUnlockCore()
{
	release_read_spinlock(&fCoreLock);
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_CPU_H
