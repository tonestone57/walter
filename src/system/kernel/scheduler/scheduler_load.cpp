/*
 * Copyright 2025, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

#include <kernel.h>
#include <scheduler_defs.h>

#include "scheduler_cpu.h"

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
static struct loadavg sAverageRunnable
	__attribute__((aligned(8))) = {{0, 0, 0}, (int32)kFScale};

// Exponential decay constants for 1, 5, and 15 minute averages.
// Formula: exp(-t / C) * kFScale, where t = 1s (update interval).
// 1m: exp(-1 / 60)  * 2048 = 2014.15 ~= 2014
// 5m: exp(-1 / 300) * 2048 = 2041.18 ~= 2041
// 15m: exp(-1 / 900) * 2048 = 2045.72 ~= 2046
const static uint64 sCExp[3] __attribute__((aligned(8))) = {2014, 2041, 2046};

static spinlock sLoadAvgLock = B_SPINLOCK_INITIALIZER;

const bigtime_t kMinMeasurementWindow = 1000;
const int kLoadClampMax = 100000;

namespace Scheduler {

int SmoothLoad(int oldLoad, int newLoad) {
	// Simple exponential smoothing
	return (oldLoad * 3 + newLoad) / 4;
}

}  // namespace Scheduler

static void _LoadavgUpdate(void* data, int iteration) {
	// Note: gTotalRunnableThreads is an instantaneous snapshot taken once
	// every 1 second.  Load spikes that begin and end within the 1-second
	// window are invisible to the EMA.  This is an inherent limitation of the
	// FreeBSD-derived algorithm (which also uses a 1-second tick), not a bug.
	// If sub-second load visibility is required in the future, the daemon
	// period must be reduced and sCExp recalibrated accordingly.
	// Optimization: Use global atomic counter instead of O(N) core scan.
	int32 threadCount = LoadAcquire(gTotalRunnableThreads);
	if (threadCount < 0)
		threadCount = 0;

	SpinLocker locker(sLoadAvgLock);
	for (int i = 0; i < 3; i++) {
		// Note: the 128-bit intermediate is correct, but the final
		// uint64 truncation can overflow for pathological thread counts or
		// if ldavg accumulated a very large value across prior iterations.
		// Clamp the result to B_INT32_MAX (a sane upper bound: no system has
		// 2^31 runnable threads).  The FreeBSD algorithm assumes the same
		// practical bound.
		// Modern GCC optimization: use uint64; intermediate fits in 64 bits.
		uint64 acc = (uint64)sCExp[i] * sAverageRunnable.ldavg[i] +
					 (uint64)threadCount * (kFScale - sCExp[i]) * kFScale;
		uint64 result = (uint64)(acc >> kFShift);
		const uint64 kMaxLdAvg = (uint64)B_INT32_MAX;
		sAverageRunnable.ldavg[i] = (result < kMaxLdAvg) ? result : kMaxLdAvg;
	}
}


status_t scheduler_loadavg_init() {
	// Note: the EMA decay constants sCExp are calibrated for a 1-second
	// update interval.
	register_kernel_daemon(_LoadavgUpdate, NULL, 1000000);
	// run the daemon every second (1,000,000 µs)

	return B_OK;
}

// #pragma mark - Syscalls

status_t _user_get_loadavg(struct loadavg* userInfo, size_t size) {
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
