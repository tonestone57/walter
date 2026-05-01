/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_COMMON_H
#define KERNEL_SCHEDULER_COMMON_H


#include <atomic>
#include <debug.h>
#include <kscheduler.h>
#include <load_tracking.h>
#include <smp.h>
#include <thread.h>
#include <user_debugger.h>
#include <util/MinMaxHeap.h>

#include "RunQueue.h"
#include "scheduler_modes.h"


//#define TRACE_SCHEDULER
#ifdef TRACE_SCHEDULER
#	define TRACE(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE(...) do { } while (false)
#endif


#define SCHEDULER_INLINE inline __attribute__((always_inline))


namespace Scheduler {

const bigtime_t kForegroundVRuntimeOffset = 5000;

struct ThreadDataVRuntimeCompare {
	template<typename ThreadData>
	bool operator()(const ThreadData* a, const ThreadData* b) const
	{
		if (a->IsRealTime()) {
			if (b->IsRealTime())
				return false;
			return true;
		}
		if (b->IsRealTime())
			return false;

		bigtime_t aRuntime = a->GetVirtualRuntime();
		if (a->IsForeground())
			aRuntime -= kForegroundVRuntimeOffset;

		bigtime_t bRuntime = b->GetVirtualRuntime();
		if (b->IsForeground())
			bRuntime -= kForegroundVRuntimeOffset;

		// Use signed delta to handle potential wrap-around or
		// comparison near the zero-boundary robustly.
		return (int64)(aRuntime - bRuntime) < 0;
	}
};

#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv64__)
	// 64-bit systems: supports up to 64 L3 domains per node
	typedef uint64 native_cpu_mask_t;
	#define SCHEDULER_MASK_IS_64_BIT 1
#else
	// 32-bit systems: supports up to 32 L3 domains per node
	// (Self-limiting: 32-bit OS RAM limits prevent massive topology anyway)
	typedef uint32 native_cpu_mask_t;
	#define SCHEDULER_MASK_IS_64_BIT 0
#endif

// Helpers for atomic operations and bit manipulation on native_cpu_mask_t
static inline native_cpu_mask_t
scheduler_atomic_or(native_cpu_mask_t* value, native_cpu_mask_t orValue)
{
#if SCHEDULER_MASK_IS_64_BIT
	return (native_cpu_mask_t)atomic_or64((int64*)value, (int64)orValue);
#else
	return (native_cpu_mask_t)atomic_or((int32*)value, (int32)orValue);
#endif
}

static inline native_cpu_mask_t
scheduler_atomic_and(native_cpu_mask_t* value, native_cpu_mask_t andValue)
{
#if SCHEDULER_MASK_IS_64_BIT
	return (native_cpu_mask_t)atomic_and64((int64*)value, (int64)andValue);
#else
	return (native_cpu_mask_t)atomic_and((int32*)value, (int32)andValue);
#endif
}

static inline native_cpu_mask_t
scheduler_atomic_get(native_cpu_mask_t* value)
{
#if SCHEDULER_MASK_IS_64_BIT
	return (native_cpu_mask_t)atomic_get64((int64*)value);
#else
	return (native_cpu_mask_t)atomic_get((int32*)value);
#endif
}

static inline int
scheduler_popcount(native_cpu_mask_t value)
{
#if SCHEDULER_MASK_IS_64_BIT
	return __builtin_popcountll(value);
#else
	return __builtin_popcount(value);
#endif
}

static inline int
scheduler_ctz(native_cpu_mask_t value)
{
#if SCHEDULER_MASK_IS_64_BIT
	return __builtin_ctzll(value);
#else
	return __builtin_ctz(value);
#endif
}


class CPUEntry;
class CoreEntry;

const int kLowLoad = kMaxLoad * 20 / 100;
const int kTargetLoad = kMaxLoad * 55 / 100;
const int kHighLoad = kMaxLoad * 70 / 100;
const int kMediumLoad = (kHighLoad + kTargetLoad) / 2;
const int kVeryHighLoad = (kMaxLoad + kHighLoad) / 2;

const int kLoadDifference = kMaxLoad * 20 / 100;

const int32 kDefaultCapacity = 1024;
const int32 kDefaultCapacityShift = 10;
const int32 kRandomSearchThreshold = 32;

// Named constant for the per-package core scan threshold.
// Switch to random sampling inside a single package when it holds more than
// this many registered cores.  Kept lower than kRandomSearchThreshold because
// individual packages contain far fewer cores than the global package count,
// and a linear scan of <= 8 cores is always inexpensive.
const int32 kRandomCoreSearchThreshold = 8;

// Maximum number of packages to scan in O(1)-bounded fallback paths.
// Referenced by GetLeastIdlePackage (scheduler_cpu.h) and the choose_core /
// rebalance / rebalance_irqs functions in low_latency.cpp and power_saving.cpp.
const int32 kMaxFallbackAttempts = 64;

const bigtime_t kCacheExpire = 15000;

extern bool gSingleCore;
extern bool gTrackCoreLoad;
extern bool gTrackCPULoad;
extern int32 gRandomSamples;

extern const bigtime_t kMinMeasurementWindow;
extern const int kLoadClampMax;

int SmoothLoad(int oldLoad, int newLoad);

extern int64 gDeadlineBucketSize;

extern std::atomic<int> gTotalRunnableThreads;
extern std::atomic<uint64_t> gIdleMask;

extern CoreType gMinCoreType;
extern CoreType gMaxCoreType;


#define ASSERT_SCHED_LOCK() ASSERT(SchedulerLockHeld())

#ifdef DEBUG_SCHEDULER
#define SCHED_ASSERT(x) ASSERT(x)
#else
#define SCHED_ASSERT(x) ((void)0)
#endif


inline void
AssertThreadReady(Thread* thread)
{
	SCHED_ASSERT(thread != nullptr);
	SCHED_ASSERT(thread->state == B_THREAD_READY);
	SCHED_ASSERT(!thread->inRunQueue);
}


inline void
AssertThreadQueued(Thread* thread)
{
	SCHED_ASSERT(thread != nullptr);
	SCHED_ASSERT(thread->inRunQueue);
}


inline int
LoadAcquire(const std::atomic<int>& value)
{
	return value.load(std::memory_order_acquire);
}


inline void
StoreRelease(std::atomic<int>& value, int v)
{
	value.store(v, std::memory_order_release);
}


inline void
AddRelease(std::atomic<int>& value, int v)
{
	value.fetch_add(v, std::memory_order_release);
}


inline void
SubAcquireRelease(std::atomic<int>& value, int v)
{
	value.fetch_sub(v, std::memory_order_acq_rel);
}


inline void
SetCPUIDle(std::atomic<uint64_t>& mask, int cpu)
{
	mask.fetch_or(1ULL << cpu, std::memory_order_release);
}


inline void
ClearCPUIDle(std::atomic<uint64_t>& mask, int cpu)
{
	mask.fetch_and(~(1ULL << cpu), std::memory_order_release);
}


inline bool
IsCPUIDle(const std::atomic<uint64_t>& mask, int cpu)
{
	return (mask.load(std::memory_order_acquire) & (1ULL << cpu)) != 0;
}


struct SchedulerSnapshot {
	int totalRunnable;
	uint64_t idleMask;
};


SchedulerSnapshot TakeSnapshot();


inline SchedulerSnapshot
MakeSchedulerSnapshot(const std::atomic<int>& total,
	const std::atomic<uint64_t>& idleMask)
{
	SchedulerSnapshot s;
	s.totalRunnable = total.load(std::memory_order_acquire);
	s.idleMask = idleMask.load(std::memory_order_acquire);
	return s;
}


inline bool
ShouldMigrate(int sourceLoad, int targetLoad, int threshold)
{
	return sourceLoad > targetLoad + threshold;
}


inline bool
ShouldReschedule(bigtime_t now, bigtime_t last, bigtime_t cooldown)
{
	return (now - last) > cooldown;
}


// True when at least one CORE_TYPE_STANDARD core exists. Used by choose_core
// to decide whether a STANDARD-core intermediate fallback is available for
// 3-type systems (EFFICIENCY + STANDARD + PERFORMANCE).
extern bool gHasStandardCores;


void init_debug_commands();
void scheduler_update_interaction_state();
bool enqueue_safe(struct Thread* thread);


class Scheduler {
public:
	static inline void SetOperationMode(scheduler_mode mode,
		scheduler_mode_operations* operations)
	{
		sCurrentMode = operations;
		sCurrentModeID = mode;
	}

	// expose sCurrentMode via a public accessor.
	// ThreadData::ComputeQuantum lives in scheduler_thread.cpp, outside
	// the Scheduler class, and needs to cache the pointer once to guard
	// against mid-quantum mode switches.  Direct access to a private
	// static member from a non-member function is ill-formed in C++.
	static inline scheduler_mode_operations* GetCurrentMode()
	{
		return sCurrentMode;
	}

	static inline scheduler_mode Mode()
	{
		return sCurrentModeID;
	}

	static inline void SwitchToMode()
	{
		sCurrentMode->switch_to_mode();
	}

	static inline void SetCPUEnabled(int32 cpu, bool enabled)
	{
		sCurrentMode->set_cpu_enabled(cpu, enabled);
	}

	static inline bool HasCacheExpired(const ThreadData* threadData)
	{
		return sCurrentMode->has_cache_expired(threadData);
	}

	static inline CoreEntry* ChooseCore(const ThreadData* threadData)
	{
		return sCurrentMode->choose_core(threadData);
	}

	static inline CoreEntry* Rebalance(const ThreadData* threadData)
	{
		return sCurrentMode->rebalance(threadData);
	}

	static inline void RebalanceIRQs(bool idle)
	{
		sCurrentMode->rebalance_irqs(idle);
	}

	static inline bigtime_t BaseQuantum()
	{
		return sCurrentMode->base_quantum;
	}

	static inline bigtime_t MinimalQuantum()
	{
		return sCurrentMode->minimal_quantum;
	}

	static inline bigtime_t QuantumMultiplier(int index)
	{
		return sCurrentMode->quantum_multipliers[index];
	}

	static inline bigtime_t MaximumLatency()
	{
		return sCurrentMode->maximum_latency;
	}

	static inline bool IsAllEnabledMask(const CPUSet& mask)
	{
		const int32 kCPUSetArraySize = (SMP_MAX_CPUS + 31) / 32;
		for (int32 i = 0; i < kCPUSetArraySize; i++) {
			if (mask.Bits(i) != gCPUEnabled.Bits(i))
				return false;
		}
		return true;
	}

private:
	static scheduler_mode sCurrentModeID;
	static scheduler_mode_operations* sCurrentMode;
};


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_COMMON_H
