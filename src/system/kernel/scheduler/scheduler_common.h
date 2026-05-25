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
#include <util/BitUtils.h>
#include <util/MinMaxHeap.h>
#include <util/atomic.h>

// #define TRACE_SCHEDULER
#ifdef TRACE_SCHEDULER
#define TRACE(...) dprintf_no_syslog(__VA_ARGS__)
#else
#define TRACE(...) \
	do {           \
	} while (false)
#endif

#define SCHEDULER_INLINE inline __attribute__((always_inline))

namespace Scheduler {

// Type-safe atomic wrappers with Acquire/Release semantics.

template <typename T>
inline int32 LoadAcquire(const T volatile& value) {
	static_assert(sizeof(T) == 4, "LoadAcquire requires a 32-bit type");
	int32 v = ::atomic_get((int32 volatile*)&value);
	memory_read_barrier();
	return v;
}

template <typename T>
inline void StoreRelease(T volatile& value, int32 v) {
	static_assert(sizeof(T) == 4, "StoreRelease requires a 32-bit type");
	memory_write_barrier();
	::atomic_set((int32 volatile*)&value, v);
}

template <typename T>
inline int32 AddRelease(T volatile& value, int32 v) {
	static_assert(sizeof(T) == 4, "AddRelease requires a 32-bit type");
	memory_write_barrier();
	int32 old = ::atomic_add((int32 volatile*)&value, v);
	memory_read_barrier();
	return old;
}

template <typename T>
inline int32 AtomicOr(T volatile& value, int32 orValue) {
	static_assert(sizeof(T) == 4, "AtomicOr requires a 32-bit type");
	return ::atomic_or((int32 volatile*)&value, orValue);
}

template <typename T>
inline int32 AtomicAnd(T volatile& value, int32 andValue) {
	static_assert(sizeof(T) == 4, "AtomicAnd requires a 32-bit type");
	return ::atomic_and((int32 volatile*)&value, andValue);
}

template <typename T>
inline int32 AtomicTestAndSet(T volatile& value, int32 newValue,
							  int32 expectedValue) {
	static_assert(sizeof(T) == 4, "AtomicTestAndSet requires a 32-bit type");
	return ::atomic_test_and_set((int32 volatile*)&value, newValue,
								 expectedValue);
}

template <typename T>
inline int32 AtomicGetAndSet(T volatile& value, int32 newValue) {
	static_assert(sizeof(T) == 4, "AtomicGetAndSet requires a 32-bit type");
	return ::atomic_get_and_set((int32 volatile*)&value, newValue);
}

// 64-bit variants

template <typename T>
inline int64 LoadAcquire64(const T volatile& value) {
	static_assert(sizeof(T) == 8, "LoadAcquire64 requires a 64-bit type");
	int64 v = ::atomic_get64((int64 volatile*)&value);
	memory_read_barrier();
	return v;
}

template <typename T>
inline void StoreRelease64(T volatile& value, int64 v) {
	static_assert(sizeof(T) == 8, "StoreRelease64 requires a 64-bit type");
	memory_write_barrier();
	::atomic_set64((int64 volatile*)&value, v);
}

template <typename T>
inline int64 AddRelease64(T volatile& value, int64 v) {
	static_assert(sizeof(T) == 8, "AddRelease64 requires a 64-bit type");
	memory_write_barrier();
	int64 old = ::atomic_add64((int64 volatile*)&value, v);
	memory_read_barrier();
	return old;
}

template <typename T>
inline int64 AtomicOr64(T volatile& value, int64 orValue) {
	static_assert(sizeof(T) == 8, "AtomicOr64 requires a 64-bit type");
	return ::atomic_or64((int64 volatile*)&value, orValue);
}

template <typename T>
inline int64 AtomicAnd64(T volatile& value, int64 andValue) {
	static_assert(sizeof(T) == 8, "AtomicAnd64 requires a 64-bit type");
	return ::atomic_and64((int64 volatile*)&value, andValue);
}

template <typename T>
inline int64 AtomicTestAndSet64(T volatile& value, int64 newValue,
								int64 expectedValue) {
	static_assert(sizeof(T) == 8, "AtomicTestAndSet64 requires a 64-bit type");
	return ::atomic_test_and_set64((int64 volatile*)&value, newValue,
								   expectedValue);
}

// Portability helpers for modern GCC and architecture independence.

#if B_HAIKU_64_BIT
typedef uint64 native_cpu_mask_t __attribute__((aligned(8)));
#define SCHEDULER_MASK_IS_64_BIT 1
#else
typedef uint32 native_cpu_mask_t;
#define SCHEDULER_MASK_IS_64_BIT 0
#endif

// Helpers for atomic operations on native_cpu_mask_t

static inline native_cpu_mask_t cpu_mask_or_atomic(native_cpu_mask_t volatile* value,
													native_cpu_mask_t orValue) {
#if SCHEDULER_MASK_IS_64_BIT
	return (native_cpu_mask_t)AtomicOr64(*value, (int64)orValue);
#else
	return (native_cpu_mask_t)AtomicOr(*value, (int32)orValue);
#endif
}

static inline native_cpu_mask_t cpu_mask_and_atomic(native_cpu_mask_t volatile* value,
													native_cpu_mask_t andValue) {
#if SCHEDULER_MASK_IS_64_BIT
	return (native_cpu_mask_t)AtomicAnd64(*value, (int64)andValue);
#else
	return (native_cpu_mask_t)AtomicAnd(*value, (int32)andValue);
#endif
}

static inline native_cpu_mask_t cpu_mask_get_atomic(native_cpu_mask_t volatile* value) {
#if SCHEDULER_MASK_IS_64_BIT
	return (native_cpu_mask_t)LoadAcquire64(*value);
#else
	return (native_cpu_mask_t)LoadAcquire(*value);
#endif
}

static inline void cpu_mask_set_atomic(native_cpu_mask_t volatile* value,
										native_cpu_mask_t newValue) {
#if SCHEDULER_MASK_IS_64_BIT
	StoreRelease64(*value, (int64)newValue);
#else
	StoreRelease(*value, (int32)newValue);
#endif
}

static inline native_cpu_mask_t cpu_mask_test_and_set_atomic(
	native_cpu_mask_t volatile* value, native_cpu_mask_t newValue,
	native_cpu_mask_t expectedValue) {
#if SCHEDULER_MASK_IS_64_BIT
	return (native_cpu_mask_t)AtomicTestAndSet64(*value,
		(int64)newValue, (int64)expectedValue);
#else
	return (native_cpu_mask_t)AtomicTestAndSet(*value,
		(int32)newValue, (int32)expectedValue);
#endif
}


static inline int scheduler_ffs(uint32 value) { return __builtin_ffs(value); }
static inline int scheduler_ffs64(uint64 value) { return __builtin_ffsll(value); }

static inline int scheduler_flsnative(native_cpu_mask_t value) {
	if (value == 0) return 0;
#if SCHEDULER_MASK_IS_64_BIT
	return 64 - __builtin_clzll(value);
#else
	return 32 - __builtin_clz(value);
#endif
}

static inline int scheduler_popcount(native_cpu_mask_t value) {
#if SCHEDULER_MASK_IS_64_BIT
	return __builtin_popcountll(value);
#else
	return __builtin_popcount(value);
#endif
}

static inline int scheduler_ctz(native_cpu_mask_t value) {
	if (value == 0) return -1;
#if SCHEDULER_MASK_IS_64_BIT
	return __builtin_ctzll(value);
#else
	return __builtin_ctz(value);
#endif
}

// atomic_pointer: architecture-independent atomic pointer operations.
template <typename T>
static inline T* AtomicPointerGet(T* const volatile* pointer) {
#if SCHEDULER_MASK_IS_64_BIT
	return reinterpret_cast<T*>(LoadAcquire64(*(int64 volatile*)pointer));
#else
	return reinterpret_cast<T*>(LoadAcquire(*(int32 volatile*)pointer));
#endif
}

template <typename T>
static inline void AtomicPointerSet(T* volatile* pointer, T* value) {
#if SCHEDULER_MASK_IS_64_BIT
	StoreRelease64(*(int64 volatile*)pointer, (int64)reinterpret_cast<addr_t>(value));
#else
	StoreRelease(*(int32 volatile*)pointer, reinterpret_cast<int32>(value));
#endif
}

template <typename T>
static inline T* AtomicPointerTestAndSet(T* volatile* pointer, T* newValue,
											 T* expectedValue) {
#if SCHEDULER_MASK_IS_64_BIT
	return reinterpret_cast<T*>(AtomicTestAndSet64(*(int64 volatile*)pointer,
		(int64)reinterpret_cast<addr_t>(newValue),
		(int64)reinterpret_cast<addr_t>(expectedValue)));
#else
	return reinterpret_cast<T*>(AtomicTestAndSet(*(int32 volatile*)pointer,
		reinterpret_cast<int32>(newValue),
		reinterpret_cast<int32>(expectedValue)));
#endif
}

extern int64 gDeadlineBucketSize __attribute__((aligned(8)));
extern uint64 gDeadlineBucketReciprocal __attribute__((aligned(8)));
extern int32 gDeadlineBucketShift __attribute__((aligned(8)));

}  // namespace Scheduler

#include "RunQueue.h"
#include "scheduler_modes.h"

namespace Scheduler {

const bigtime_t kForegroundVRuntimeOffset = 5000;

const bigtime_t kMaxLagFloor = 200000;
const bigtime_t kInteractiveLagCreditMax = 50000;

const bigtime_t kSVTStagnationThreshold = 40000;
const bigtime_t kSVTStagnationBuffer = 10000;

const int64 kReferenceWeight = 1000000;

extern int64 gL3LagThreshold;
extern int64 gNUMANodeLagThreshold;
extern int64 gGlobalLagThreshold;

struct ThreadDataVRuntimeCompare {
	template <typename ThreadData>
	bool operator()(const ThreadData* a, const ThreadData* b) const {
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

		return (aRuntime - bRuntime) < 0;
	}
};

struct ThreadDataDeadlineCompare {
	template <typename ThreadData>
	bool operator()(const ThreadData* a, const ThreadData* b) const {
		bool aRT = a->IsRealTime();
		bool bRT = b->IsRealTime();
		if (aRT != bRT)
			return aRT;

		if (aRT)
			return a->GetPriority() > b->GetPriority();

		bigtime_t aDeadline = a->GetVirtualDeadline();
		bigtime_t bDeadline = b->GetVirtualDeadline();

		return (aDeadline - bDeadline) < 0;
	}
};

struct ThreadDataLagCompare {
	ThreadDataLagCompare(bigtime_t svt = -1) : fSVT(svt) {}

	template <typename ThreadData>
	bool operator()(const ThreadData* a, const ThreadData* b) const {
		int64 lagA, lagB;
		if (fSVT == (bigtime_t)-1) {
			lagA = a->GetLag();
			lagB = b->GetLag();
		} else {
			lagA = (fSVT - a->GetVirtualRuntime()) * a->GetWeight() / 1000;
			lagB = (fSVT - b->GetVirtualRuntime()) * b->GetWeight() / 1000;
		}
		// Highest positive lag first (most under-served)
		return (lagA - lagB) > 0;
	}

private:
	bigtime_t fSVT;
};

static const int32 kWeightTable[] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0-9
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  // 10-19
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5,  // 20-29
	10, 10, 20, 30, 40, 50, 60, 70, 80, 100, // 30-39
	120, 140, 160, 180, 200, 250, 300, 350, 400, 500, // 40-49
	600, 700, 800, 900, 1000, 1200, 1400, 1600, 1800, 2000, // 50-59
	2500, 3000, 3500, 4000, 5000, 6000, 7000, 8000, 9000, 10000, // 60-69
	12000, 14000, 16000, 18000, 20000, 25000, 30000, 35000, 40000, 50000, // 70-79
	60000, 70000, 80000, 90000, 100000, 120000, 140000, 160000, 180000, 200000, // 80-89
	250000, 300000, 350000, 400000, 500000, 600000, 700000, 800000, 900000, 1000000 // 90-99
};

static inline int32 get_weight(int32 priority) {
	if (priority < 0) return 1;
	if (priority >= 100) return 1000000;
	return kWeightTable[priority];
}

struct ThreadDataOptimal {
	template <typename ThreadData>
	bool operator()(const ThreadData* /*thread*/) const {
		return true;
	}
};

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

const int32 kRandomCoreSearchThreshold = 8;
const int32 kMaxFallbackAttempts = 64;

const bigtime_t kCacheExpire = 15000;

extern bool gSingleCore;
extern bool gTrackCoreLoad;
extern bool gTrackCPULoad;
extern int32 gRandomSamples;

extern const bigtime_t kMinMeasurementWindow __attribute__((aligned(8)));
extern const int kLoadClampMax;

extern int32 scheduler_get_total_runnable_threads();
extern CPUSet gIdleMask;

extern int64 gRCUGeneration __attribute__((aligned(8)));
extern spinlock gSchedulerUpdateLock;

struct rcu_callback {
	void (*callback)(void*);
	void* arg;
	int64 targetGen;
	struct rcu_callback* next;
};

void scheduler_synchronize();
void scheduler_call_rcu(void (*callback)(void*), void* arg);

extern CoreType gMinCoreType;
extern CoreType gMaxCoreType;

#define ASSERT_SCHED_LOCK() ASSERT(SchedulerLockHeld())

#ifdef DEBUG_SCHEDULER
#define SCHED_ASSERT(x) ASSERT(x)
#else
#define SCHED_ASSERT(x) ((void)0)
#endif

inline void AssertThreadReady(Thread* thread) {
	SCHED_ASSERT(thread != NULL);
	SCHED_ASSERT(thread->state == B_THREAD_READY);
	SCHED_ASSERT(!thread->inRunQueue);
}

inline void AssertThreadQueued(Thread* thread) {
	SCHED_ASSERT(thread != NULL);
	SCHED_ASSERT(thread->inRunQueue);
}

inline void SetCPUIDle(CPUSet& mask, int cpu) {
	mask.SetBitAtomic(cpu);
}

inline void ClearCPUIDle(CPUSet& mask, int cpu) {
	mask.ClearBitAtomic(cpu);
}

inline bool IsCPUIDle(const CPUSet& mask, int cpu) {
	return mask.GetBit(cpu);
}

struct SchedulerSnapshot {
	int32 totalRunnable;
	CPUSet idleMask;
};

inline SchedulerSnapshot MakeSchedulerSnapshot(int32 total,
											   const CPUSet& idleMask) {
	SchedulerSnapshot s;
	s.totalRunnable = total;

	const int32 kWords = (SMP_MAX_CPUS + 31) / 32;
	const uint32* bits = idleMask.BitData();
	for (int32 i = 0; i < kWords; i++) {
		s.idleMask.SetWord(i, (uint32)LoadAcquire(bits[i]));
	}
	return s;
}

inline SchedulerSnapshot TakeSnapshot() {
	return MakeSchedulerSnapshot(scheduler_get_total_runnable_threads(), gIdleMask);
}

inline bool ShouldMigrate(int sourceLoad, int targetLoad, int threshold) {
	return sourceLoad > targetLoad + threshold;
}

inline bool ShouldReschedule(bigtime_t now, bigtime_t last,
							 bigtime_t cooldown) {
	return (now - last) > cooldown;
}

extern bool gHasStandardCores;

void init_debug_commands();
void scheduler_update_interaction_state(bigtime_t now = 0);
bool enqueue_safe(struct Thread* thread, bigtime_t now = 0);

class Scheduler {
public:
	static inline void SetOperationMode(scheduler_mode mode,
										scheduler_mode_operations* operations) {
		AtomicPointerSet<scheduler_mode_operations>(&sCurrentMode,
													  operations);
		StoreRelease(sCurrentModeID, (int32)mode);
	}

	static inline scheduler_mode_operations* GetCurrentMode() {
		return AtomicPointerGet<scheduler_mode_operations>(&sCurrentMode);
	}

	static inline scheduler_mode Mode() {
		return (scheduler_mode)LoadAcquire(sCurrentModeID);
	}

	static inline void SwitchToMode() { GetCurrentMode()->switch_to_mode(); }

	static inline void SetCPUEnabled(int32 cpu, bool enabled) {
		GetCurrentMode()->set_cpu_enabled(cpu, enabled);
	}

	static inline bool HasCacheExpired(const ThreadData* threadData,
									   bigtime_t now) {
		return GetCurrentMode()->has_cache_expired(threadData, now);
	}

	static inline CoreEntry* ChooseCore(const ThreadData* threadData,
										const CPUSet& mask, bigtime_t now) {
		return GetCurrentMode()->choose_core(threadData, mask, now);
	}

	static inline CoreEntry* Rebalance(const ThreadData* threadData,
									   const CPUSet& mask, bigtime_t now) {
		return GetCurrentMode()->rebalance(threadData, mask, now);
	}

	static inline void RebalanceIRQs(bool idle) {
		GetCurrentMode()->rebalance_irqs(idle);
	}

	static inline bigtime_t BaseQuantum() {
		return GetCurrentMode()->base_quantum;
	}

	static inline bigtime_t MinimalQuantum() {
		return GetCurrentMode()->minimal_quantum;
	}

	static inline bigtime_t QuantumMultiplier(int index) {
		return GetCurrentMode()->quantum_multipliers[index];
	}

	static inline bigtime_t MaximumLatency() {
		return GetCurrentMode()->maximum_latency;
	}

	static inline bool IsAllEnabledMask(const CPUSet& mask) {
		const int32 kCPUSetArraySize = (SMP_MAX_CPUS + 31) / 32;
		for (int32 i = 0; i < kCPUSetArraySize; i++) {
			if (mask.Bits(i) != gCPUEnabled.Bits(i))
				return false;
		}
		return true;
	}

private:
	static scheduler_mode sCurrentModeID __attribute__((aligned(8)));
	static scheduler_mode_operations* sCurrentMode __attribute__((aligned(8)));
};

}  // namespace Scheduler

#endif	// KERNEL_SCHEDULER_COMMON_H
