// AUDIT FIXES: issues 6, 12, 36, 45, 52, 59, 70, 96
/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 *
 * Audit and robustness fixes (2025).
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
const int32 kMaxCoresPerPackage = (int32)(sizeof(native_cpu_mask_t) * 8);

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

	// Index of this CPU within its core, assigned sequentially in AddCPU.
	// Used by UpdatePriorityBoostScalable for round-robin epoch ownership.
						int32			fCoreLocalIndex;

						void			PushFront(ThreadData* thread,
											int32 priority);
						void			PushBack(ThreadData* thread,
											int32 priority);
						void			Remove(ThreadData* thread);
						ThreadData*		PeekThread() const;
						ThreadData*		PeekIdleThread() const;
	inline				const ThreadRunQueue*	RunQueue() const { return &fRunQueue; }

	inline				ThreadRunQueue::ConstIterator
										GetConstIterator() const
											{ return fRunQueue.GetConstIterator(); }

						void			UpdatePriority(int32 priority);

	inline				int32			GetLoad() const;
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
											{ return atomic_get(const_cast<int32*>(&fThreadCount)); }

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
	// run queue.
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

public:
						native_cpu_mask_t	fLocalIndices __attribute__((aligned(8)));

						friend class DebugDumper;
} __attribute__((aligned(64)));

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

public:
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
	if (fCore != NULL && fCore->CPUCount() > 1) {
		// If at least one other thread is running on this core
		if (fCore->ThreadCount() > 1)
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
		if (fNode != NULL)
			fNode->PackageGoesIdle(this);
	}
}


inline void
PackageEntry::CoreWakesUp(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();

	// Clear the mask bit BEFORE decrementing the count.
	native_cpu_mask_t clearBit = (native_cpu_mask_t)1 << core->PackageIndex();
	native_cpu_mask_t oldMask = scheduler_atomic_and(&fIdleCoreMask, ~clearBit);

	atomic_add(&fIdleCoreCount, -1);

	// Detect the transition from fully-idle to partially-active.
	if ((oldMask & ~clearBit) == 0) {
		if (fNode != NULL)
			fNode->PackageWakesUp(this);
	}
}


inline void
SchedulerNode::PackageGoesIdle(PackageEntry* package)
{
	SCHEDULER_ENTER_FUNCTION();

	const int32 kMaxPackagesPerNode = (int32)(sizeof(native_cpu_mask_t) * 8);
	if (package->NodeIndex() < 0 || package->NodeIndex() >= kMaxPackagesPerNode)
		return;

	native_cpu_mask_t oldMask = scheduler_atomic_or(&fIdlePackageMask,
		(native_cpu_mask_t)1 << package->NodeIndex());

	if (oldMask == 0) {
		if (fNodeID < 64)
			atomic_or64((int64*)&gIdleNodeMask, 1ULL << fNodeID);
	}
}


inline void
SchedulerNode::PackageWakesUp(PackageEntry* package)
{
	SCHEDULER_ENTER_FUNCTION();

	const int32 kMaxPackagesPerNode = (int32)(sizeof(native_cpu_mask_t) * 8);
	if (package->NodeIndex() < 0 || package->NodeIndex() >= kMaxPackagesPerNode)
		return;

	native_cpu_mask_t clearBit = (native_cpu_mask_t)1 << package->NodeIndex();
	native_cpu_mask_t oldMask = scheduler_atomic_and(&fIdlePackageMask, ~clearBit);

	// Detect the transition from fully-idle to partially-active for the node.
	// Issue 12/19 fix: re-check fIdlePackageMask and gIdleNodeMask atomically,
	// preventing lost wake-up notifications during rapid idle/active transitions.
	if ((oldMask & clearBit) != 0 && (oldMask & ~clearBit) == (native_cpu_mask_t)0) {
		if (fNodeID < 64) {
			const int64 nodeBit = (int64)(1ULL << fNodeID);
			int64 nodeMask;
			const int kMaxWakeupRetries = 64;
			int wakeupRetries = 0;
			while (true) {
				nodeMask = atomic_get64((int64*)&gIdleNodeMask);
				if (!(nodeMask & nodeBit))
					break;

				if (scheduler_atomic_get(&fIdlePackageMask)
						!= (native_cpu_mask_t)0) {
					break;
				}

				if (atomic_test_and_set64((int64*)&gIdleNodeMask,
						nodeMask & ~nodeBit, nodeMask) == nodeMask) {
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


inline uint64
SchedulerNode::IdlePackageMask() const
{
	SCHEDULER_ENTER_FUNCTION();
	return (uint64)scheduler_atomic_get(
		const_cast<native_cpu_mask_t*>(&fIdlePackageMask));
}


inline void
CoreEntry::CPUGoesIdle(CPUEntry* cpu)
{
	if (gSingleCore)
		return;

	SetCPUIDle(gIdleMask, cpu->ID());

	DecrementTotalThreadCount();
	// Issue 36/70 fix: ensure fIdleCPUCount increments are visible before
	// evaluating fCPUCount.
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
	// Issue 36/70 fix: same memory barrier for wake-ups.
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
		for (int32 i = 0; i < kMaxFallbackAttempts; i++) {
			int32 idx = (int32)(((uint64)cpu->GetRandom() * gPackageCount) >> 32);
			PackageEntry* current = &gPackageEntries[idx];
			if (current->fNode == NULL)
				continue;
			int32 count = atomic_get((int32*)&current->fIdleCoreCount);
			if (count != 0 && (package == NULL || count < bestIdleCount)) {
				package = current;
				bestIdleCount = count;
			}
		}
	} else {
		for (int32 i = 0; i < gPackageCount; i++) {
			PackageEntry* current = &gPackageEntries[i];
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
	const uint32* bitmap = fRunQueue.GetBitmap();
	for (int i = ThreadRunQueue::kBitmapSize - 1; i >= 0; i--) {
		uint32 val = (uint32)atomic_get(
			const_cast<int32*>(reinterpret_cast<const int32*>(bitmap + i)));
		if (i == ThreadRunQueue::kBitmapSize - 1)
			val &= (uint32)((2ULL << (THREAD_MAX_SET_PRIORITY % 32)) - 1);
		if (val != 0) {
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
