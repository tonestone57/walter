// AUDIT FIXES: issues 4 and 16
/*
 * Copyright 2025, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */


#include "scheduler_cpu.h"

#include <kernel.h>
#include <scheduler_defs.h>


using namespace Scheduler;

/*
 * Load average algorithm from FreeBSD, see kern_sync.c
 *
 * It uses a fixed-point arithmetic with the scaling factor kFScale (2^kFShift).
 * The load average is an exponential moving average (EMA) of the number of
 * runnable threads.
 *
 * The constants sCExp are the exponential decay factors for 1, 5, and 15
 * minute intervals, pre-calculated with the scaling factor.
 * kFShift = 11 roughly provides 3 decimal places of precision.
 */
const static int kFShift = 11;
const static long kFScale = 1 << kFShift;
static struct loadavg sAverageRunnable = {{0, 0, 0}, kFScale};
const static uint64 sCExp[3] __attribute__((aligned(8))) = {(uint64)(0.9200444146293232 * kFScale),
	(uint64)(0.9834714538216174 * kFScale), (uint64)(0.9944598480048967 * kFScale)};

static spinlock sLoadAvgLock = B_SPINLOCK_INITIALIZER;

const bigtime_t kMinMeasurementWindow = 1000;
const int kLoadClampMax = 100000;


namespace Scheduler {


int
SmoothLoad(int oldLoad, int newLoad)
{
	// Simple exponential smoothing
	return (oldLoad * 3 + newLoad) / 4;
}


}	// namespace Scheduler




static void
_LoadavgUpdate(void *data, int iteration)
{
	// Issue 16: gTotalRunnableThreads is an instantaneous snapshot taken once
	// every 5 seconds.  Load spikes that begin and end within the 5-second
	// window are invisible to the EMA.  This is an inherent limitation of the
	// FreeBSD-derived algorithm (which also uses a 5-second tick), not a bug.
	// If sub-second load visibility is required in the future, the daemon
	// period must be reduced and sCExp recalibrated accordingly.
	// Optimization: Use global atomic counter instead of O(N) core scan.
	int32 threadCount = atomic_get(&gTotalRunnableThreads);
	if (threadCount < 0)
		threadCount = 0;

	SpinLocker locker(sLoadAvgLock);
	for (int i = 0; i < 3; i++) {
		// Issue 10 fix: the 128-bit intermediate is correct, but the final
		// uint64 truncation can overflow for pathological thread counts or
		// if ldavg accumulated a very large value across prior iterations.
		// Clamp the result to INT32_MAX (a sane upper bound: no system has
		// 2^31 runnable threads).  The FreeBSD algorithm assumes the same
		// practical bound.
		// GCC 2.95 compatibility: use uint64; intermediate fits in 64 bits.
		uint64 acc =
			(uint64)sCExp[i] * sAverageRunnable.ldavg[i]
			+ (uint64)threadCount * (kFScale - sCExp[i]) * kFScale;
		uint64 result = (uint64)(acc >> kFShift);
		const uint64 kMaxLdAvg = (uint64)INT32_MAX;
		sAverageRunnable.ldavg[i] = (result < kMaxLdAvg) ? result : kMaxLdAvg;
	}
}


status_t
scheduler_loadavg_init()
{
	// Issue 4: the EMA decay constants sCExp are calibrated for a 5-second
	// (5,000,000 µs) update interval, matching FreeBSD kern_sync.c.
	// The argument below must remain 5000000; changing it without
	// recalibrating sCExp will produce meaningless load average values.
	// static_assert replaced by comment for GCC 2.95
	// true, "verify daemon period matches EMA calibration"
	// Issue 24 fix: the loadavg EMA decay constants (sCExp) are calibrated
	// for a 5-second update interval (matching FreeBSD kern_sync.c).
	// register_kernel_daemon period is in microseconds; 5000 µs = 5 ms is
	// far too frequent and would produce meaningless EMA values.
	// Correct value: 5,000,000 µs = 5 seconds.
	register_kernel_daemon(_LoadavgUpdate, NULL, 5000000);
		// run the daemon every five seconds (5,000,000 µs)

	return B_OK;
}


// #pragma mark - Syscalls


status_t
_user_get_loadavg(struct loadavg* userInfo, size_t size)
{
	if (userInfo == NULL || !IS_USER_ADDRESS(userInfo))
		return B_BAD_ADDRESS;
	if (size != sizeof(struct loadavg))
		return B_BAD_VALUE;

	struct loadavg loadAvg;
	{
		InterruptsSpinLocker locker(sLoadAvgLock);
		loadAvg = sAverageRunnable;
	}

	if (user_memcpy(userInfo, &loadAvg, sizeof(struct loadavg)) < B_OK)
		return B_BAD_ADDRESS;

	return B_OK;
}
