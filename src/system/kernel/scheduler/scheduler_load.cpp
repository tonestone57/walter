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


static void
_LoadavgUpdate(void *data, int iteration)
{
	uint64 threadCount = 0;
	for (int i = 0; i < gCoreCount; i++)
		threadCount += gCoreEntries[i].ThreadCount();

	for (int i = 0; i < 3; i++) {
		sAverageRunnable.ldavg[i]
			= (sCExp[i] * sAverageRunnable.ldavg[i] + threadCount * kFScale * (kFScale - sCExp[i]))
			>> kFShift;
	}
}


status_t
scheduler_loadavg_init()
{
	register_kernel_daemon(_LoadavgUpdate, NULL, 5000);
		// run the daemon once five second

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
	if (user_memcpy(userInfo, &sAverageRunnable, sizeof(struct loadavg)) < B_OK)
		return B_BAD_ADDRESS;

	return B_OK;
}
