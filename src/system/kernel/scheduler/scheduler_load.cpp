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
const static uint64 sCExp[3] = {(uint64)(0.9200444146293232 * kFScale),
	(uint64)(0.9834714538216174 * kFScale), (uint64)(0.9944598480048967 * kFScale)};

static spinlock sLoadAvgLock = B_SPINLOCK_INITIALIZER;


static void
_LoadavgUpdate(void *data, int iteration)
{
	// Optimization: Use global atomic counter instead of O(N) core scan.
	int32 threadCount = atomic_get(&gTotalRunnableThreads);
	if (threadCount < 0)
		threadCount = 0;

	SpinLocker locker(sLoadAvgLock);
	for (int i = 0; i < 3; i++) {
		// Issue 11 fix: use unsigned 128-bit intermediate to prevent overflow.
		// With kFScale=2048 and threadCount up to INT32_MAX (~2^31):
		//   (uint64)threadCount * (kFScale - sCExp[i]) * kFScale
		//   <= 2^31 * 2048 * 2048 = 2^53  (fits in uint64)
		// But sCExp[i] * ldavg[i] where ldavg can accumulate to ~threadCount*kFScale:
		//   2^11 * (2^31 * 2^11) = 2^53  (fits in uint64 too, with margin)
		// The overflow is marginal today but use __uint128_t to be safe and
		// future-proof against larger kFScale or thread count expansions.
		unsigned __int128 acc =
			(unsigned __int128)sCExp[i] * sAverageRunnable.ldavg[i]
			+ (unsigned __int128)(uint64)threadCount
				* (kFScale - sCExp[i]) * kFScale;
		sAverageRunnable.ldavg[i] = (uint64)(acc >> kFShift);
	}
}


status_t
scheduler_loadavg_init()
{
	register_kernel_daemon(_LoadavgUpdate, NULL, 5000);
		// run the daemon every five seconds

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
