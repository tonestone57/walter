/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_COMMON_H
#define KERNEL_SCHEDULER_COMMON_H


#include <debug.h>
#include <kscheduler.h>
#include <load_tracking.h>
#include <smp.h>
#include <thread.h>
#include <user_debugger.h>
#include <util/atomic.h>
#include <util/BitUtils.h>
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


struct ThreadDataOptimal {
	template<typename ThreadData>
	bool operator()(const ThreadData* /*thread*/) const
	{
		return true;
	}
};


// Portability helpers for GCC 2.95 and architecture independence.
// These wrappers ensure atomic operations and bit manipulation work correctly
// on both 32-bit and 64-bit systems.

#if B_HAIKU_64_BIT
	// 64-bit systems: supports up to 64 L3 domains per node
	typedef uint64 native_cpu_mask_t __attribute__((aligned(8)));
	#define SCHEDULER_MASK_IS_64_BIT 1
#else
	// 32-bit systems: supports up to 32 L3 domains per node
	// (Self-limiting: 32-bit OS RAM limits prevent massive topology anyway)
	typedef uint32 native_cpu_mask_t;
	#define SCHEDULER_MASK_IS_64_BIT 0
#endif

// Helpers for atomic operations and bit manipulation on native_cpu_mask_t
// These wrappers provide architecture-independent atomic access (32/64-bit).

static inline native_cpu_mask_t
scheduler_atomic_or(native_cpu_mask_t* value, native_cpu_mask_t orValue)
{
#if SCHEDULER_MASK_IS_64_BIT
	return (native_cpu_mask_t)atomic_or64((int64 volatile*)value, (int64)orValue);
#else
	return (native_cpu_mask_t)atomic_or((int32 volatile*)value, (int32)orValue);
#endif
}


static inline native_cpu_mask_t
scheduler_atomic_and(native_cpu_mask_t* value, native_cpu_mask_t andValue)
{
#if SCHEDULER_MASK_IS_64_BIT
	return (native_cpu_mask_t)atomic_and64((int64 volatile*)value, (int64)andValue);
#else
	return (native_cpu_mask_t)atomic_and((int32 volatile*)value, (int32)andValue);
#endif
}


static inline native_cpu_mask_t
scheduler_atomic_get(native_cpu_mask_t* value)
{
#if SCHEDULER_MASK_IS_64_BIT
	return (native_cpu_mask_t)atomic_get64((int64 volatile*)value);
#else
	return (native_cpu_mask_t)atomic_get((int32 volatile*)value);
#endif
}

static inline void
scheduler_atomic_set(native_cpu_mask_t* value, native_cpu_mask_t newValue)
{
#if SCHEDULER_MASK_IS_64_BIT
	atomic_set64((int64 volatile*)value, (int64)newValue);
#else
	atomic_set((int32 volatile*)value, (int32)newValue);
#endif
}

static inline native_cpu_mask_t
scheduler_atomic_test_and_set(native_cpu_mask_t* value, native_cpu_mask_t newValue,
	native_cpu_mask_t expectedValue)
{
#if SCHEDULER_MASK_IS_64_BIT
	return (native_cpu_mask_t)atomic_test_and_set64((int64 volatile*)value,
		(int64)newValue, (int64)expectedValue);
#else
	return (native_cpu_mask_t)atomic_test_and_set((int32 volatile*)value,
		(int32)newValue, (int32)expectedValue);
#endif
}

static inline int
scheduler_popcount(native_cpu_mask_t value)
{
#if SCHEDULER_MASK_IS_64_BIT
	return (int)(count_set_bits((uint32)value)
		+ count_set_bits((uint32)(value >> 32)));
#else
	return (int)count_set_bits((uint32)value);
#endif
}

// scheduler_ffs: portable Find First Set bit for GCC 2.95.
// Returns 1-based index of the first bit set, or 0 if value is 0.
static inline int
scheduler_ffs(uint32 value)
{
	if (value == 0)
		return 0;
	// fls(x & -x) is correct for isolating and finding the lowest bit.
	return (int)fls(value & (uint32)-(int32)value);
}

static inline int
scheduler_ffs64(uint64 value)
{
	uint32 low = (uint32)value;
	if (low != 0)
		return scheduler_ffs(low);
	uint32 high = (uint32)(value >> 32);
	int bit = scheduler_ffs(high);
	if (bit == 0)
		return 0;
	return bit + 32;
}

// scheduler_ctz: portable Count Trailing Zeros for GCC 2.95.
static inline int
scheduler_ctz(native_cpu_mask_t value)
{
	if (value == 0)
		return -1;
#if SCHEDULER_MASK_IS_64_BIT
	return scheduler_ffs64((uint64)value) - 1;
#else
	return scheduler_ffs((uint32)value) - 1;
#endif
}


// atomic_pointer: architecture-independent atomic pointer operations.
// Necessary for GCC 2.95 compatibility and 32/64-bit portability.
template<typename T>
static inline T*
atomic_pointer_get(T* volatile* pointer)
{
#if SCHEDULER_MASK_IS_64_BIT
	T* value = (T*)atomic_get64((int64 volatile*)pointer);
#else
	T* value = (T*)atomic_get((int32 volatile*)pointer);
#endif
	memory_read_barrier();
	return value;
}


template<typename T>
static inline void
atomic_pointer_set(T* volatile* pointer, T* value)
{
	memory_write_barrier();
#if SCHEDULER_MASK_IS_64_BIT
	atomic_set64((int64 volatile*)pointer, (int64)value);
#else
	atomic_set((int32 volatile*)pointer, (int32)value);
#endif
}


template<typename T>
static inline T*
atomic_pointer_test_and_set(T* volatile* pointer, T* newValue, T* expectedValue)
{
#if SCHEDULER_MASK_IS_64_BIT
	return (T*)atomic_test_and_set64((int64 volatile*)pointer, (int64)newValue,
		(int64)expectedValue);
#else
	return (T*)atomic_test_and_set((int32 volatile*)pointer, (int32)newValue,
		(int32)expectedValue);
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

const int kSMTPenalty = 2;

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

extern const bigtime_t kMinMeasurementWindow __attribute__((aligned(8)));
extern const int kLoadClampMax;


extern int64 gDeadlineBucketSize __attribute__((aligned(8)));

extern int32 gTotalRunnableThreads;
extern uint64 gIdleMask __attribute__((aligned(8)));

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
	SCHED_ASSERT(thread != NULL);
	SCHED_ASSERT(thread->state == B_THREAD_READY);
	SCHED_ASSERT(!thread->inRunQueue);
}


inline void
AssertThreadQueued(Thread* thread)
{
	SCHED_ASSERT(thread != NULL);
	SCHED_ASSERT(thread->inRunQueue);
}


inline int
LoadAcquire(const int32& value)
{
	return atomic_get(const_cast<int32*>(&value));
}


inline void
StoreRelease(int32& value, int v)
{
	atomic_set(&value, v);
}


inline void
AddRelease(int32& value, int v)
{
	atomic_add(&value, v);
}


inline void
SubAcquireRelease(int32& value, int v)
{
	atomic_add(&value, -v);
}


inline void
SetCPUIDle(uint64& mask, int cpu)
{
	if ((unsigned)cpu >= 64)
		return;
	atomic_or64((int64*)&mask, 1ULL << cpu);
}


inline void
ClearCPUIDle(uint64& mask, int cpu)
{
	if ((unsigned)cpu >= 64)
		return;
	atomic_and64((int64*)&mask, ~(1ULL << cpu));
}


inline bool
IsCPUIDle(const uint64& mask, int cpu)
{
	if ((unsigned)cpu >= 64)
		return false;
	return (atomic_get64((int64*)&mask) & (1ULL << cpu)) != 0;
}


struct SchedulerSnapshot {
	int32 totalRunnable;
	uint64 idleMask __attribute__((aligned(8)));
};


SchedulerSnapshot TakeSnapshot();


inline SchedulerSnapshot
MakeSchedulerSnapshot(const int32& total,
	const uint64& idleMask)
{
	SchedulerSnapshot s;
	s.totalRunnable = atomic_get(const_cast<int32*>(&total));
	s.idleMask = atomic_get64((int64*)&idleMask);
	return s;
}


inline bool
ShouldMigrate(int sourceLoad, int targetLoad, int threshold)
{
	// Issue 80
	return sourceLoad > targetLoad + threshold;
}


inline bool
ShouldReschedule(bigtime_t now, bigtime_t last, bigtime_t cooldown)
{
	return (now - last) > cooldown; // Issue 81 fix: reschedule check
}


// True when at least one CORE_TYPE_STANDARD core exists. Used by choose_core
// to decide whether a STANDARD-core intermediate fallback is available for
// 3-type systems (EFFICIENCY + STANDARD + PERFORMANCE).
extern bool gHasStandardCores;


void init_debug_commands();
void scheduler_update_interaction_state(bigtime_t now = 0);
bool enqueue_safe(struct Thread* thread, bigtime_t now = 0);


class Scheduler {
public:
	static inline void SetOperationMode(scheduler_mode mode,
		scheduler_mode_operations* operations)
	{
		atomic_pointer_set<scheduler_mode_operations>(&sCurrentMode, operations); // Issue 87 fix: atomic mode set
		atomic_set((int32*)&sCurrentModeID, (int32)mode);
	}

	// expose sCurrentMode via a public accessor.
	// ThreadData::ComputeQuantum lives in scheduler_thread.cpp, outside
	// the Scheduler class, and needs to cache the pointer once to guard
	// against mid-quantum mode switches.  Direct access to a private
	// static member from a non-member function is ill-formed in C++.
	static inline scheduler_mode_operations* GetCurrentMode()
	{
		return atomic_pointer_get<scheduler_mode_operations>(&sCurrentMode);
	}

	static inline scheduler_mode Mode()
	{
		return (scheduler_mode)atomic_get((int32*)&sCurrentModeID);
	}

	static inline void SwitchToMode()
	{
		GetCurrentMode()->switch_to_mode();
	}

	static inline void SetCPUEnabled(int32 cpu, bool enabled)
	{
		GetCurrentMode()->set_cpu_enabled(cpu, enabled);
	}

	static inline bool HasCacheExpired(const ThreadData* threadData)
	{
		return GetCurrentMode()->has_cache_expired(threadData);
	}

	static inline CoreEntry* ChooseCore(const ThreadData* threadData,
		const CPUSet& mask)
	{
		return GetCurrentMode()->choose_core(threadData, mask);
	}

	static inline CoreEntry* Rebalance(const ThreadData* threadData,
		const CPUSet& mask)
	{
		return GetCurrentMode()->rebalance(threadData, mask);
	}

	static inline void RebalanceIRQs(bool idle)
	{
		GetCurrentMode()->rebalance_irqs(idle);
	}

	static inline bigtime_t BaseQuantum()
	{
		return GetCurrentMode()->base_quantum;
	}

	static inline bigtime_t MinimalQuantum()
	{
		return GetCurrentMode()->minimal_quantum;
	}

	static inline bigtime_t QuantumMultiplier(int index)
	{
		return GetCurrentMode()->quantum_multipliers[index];
	}

	static inline bigtime_t MaximumLatency()
	{
		return GetCurrentMode()->maximum_latency;
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
