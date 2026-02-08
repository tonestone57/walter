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
#include <util/MinMaxHeap.h>

#include "RunQueue.h"


//#define TRACE_SCHEDULER
#ifdef TRACE_SCHEDULER
#	define TRACE(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE(...) do { } while (false)
#endif


#define SCHEDULER_INLINE inline __attribute__((always_inline))


namespace Scheduler {

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
const int32 kRandomSearchThreshold = 8;

const bigtime_t kCacheExpire = 15000;

extern bool gSingleCore;
extern bool gTrackCoreLoad;
extern bool gTrackCPULoad;
extern int32 gRandomSamples;


void init_debug_commands();


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_COMMON_H
