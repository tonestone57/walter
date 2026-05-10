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
static struct loadavg sAverageRunnable __attribute__((aligned(8))) = {{0, 0, 0}, (int32)kFScale};

// Exponential decay constants for 1, 5, and 15 minute averages.
// Formula: exp(-t / C) * kFScale, where t = 1s (update interval).
// 1m: exp(-1 / 60)  * 2048 = 2014.15 ~= 2014
// 5m: exp(-1 / 300) * 2048 = 2041.18 ~= 2041
// 15m: exp(-1 / 900) * 2048 = 2045.72 ~= 2046
const static uint64 sCExp[3] __attribute__((aligned(8))) = { 2014, 2041, 2046 };

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
	// Issue 16 fix: increased load average resolution to 1 second.
	// gTotalRunnableThreads is an instantaneous snapshot taken once
	// every second.  Load spikes that begin and end within the 1-second
	// window are invisible to the EMA, but 1s visibility is superior to
	// the standard FreeBSD 5s interval for interactive workloads.
	// Optimization: Use global atomic counter instead of O(N) core scan.
	int32 threadCount = atomic_get(&gTotalRunnableThreads);
	if (threadCount < 0)
		threadCount = 0;

	SpinLocker locker(sLoadAvgLock);
	for (int i = 0; i < 3; i++) {
		// Issue 10 fix: the 128-bit intermediate is correct, but the final
		// uint64 truncation can overflow for pathological thread counts or
		// if ldavg accumulated a very large value across prior iterations.
		// Clamp the result to B_INT32_MAX (a sane upper bound: no system has
		// 2^31 runnable threads).  The FreeBSD algorithm assumes the same
		// practical bound.
		// GCC 2.95 compatibility: use uint64; intermediate fits in 64 bits.
		uint64 acc =
			(uint64)sCExp[i] * sAverageRunnable.ldavg[i]
			+ (uint64)threadCount * (kFScale - sCExp[i]) * kFScale;
		uint64 result = (uint64)(acc >> kFShift);
		const uint64 kMaxLdAvg = (uint64)B_INT32_MAX;
		sAverageRunnable.ldavg[i] = (result < kMaxLdAvg) ? result : kMaxLdAvg;
	}
}


status_t
scheduler_loadavg_init()
{
	// Issue 4/16/24 fix: calibrated for 1-second (1,000,000 µs) update interval.
	// High resolution load tracking improves visibility of short-lived bursts.
	// sCExp constants are recalibrated for this 1s period.
	register_kernel_daemon(_LoadavgUpdate, NULL, 1000000);
		// run the daemon every second (1,000,000 µs)

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
