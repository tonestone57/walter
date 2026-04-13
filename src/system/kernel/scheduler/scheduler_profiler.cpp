/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_profiler.h"

#include <debug.h>
#include <util/AutoLock.h>

#include <algorithm>


#ifdef SCHEDULER_PROFILING


using namespace Scheduler;
using namespace Scheduler::Profiling;


static Profiler* sProfiler;

static int dump_profiler(int argc, char** argv);


Profiler::Profiler()
	:
	kMaxFunctionEntries(1024),
	kMaxFunctionStackEntries(512),
	fFunctionData(new(std::nothrow) FunctionData[kMaxFunctionEntries]),
	fSortBuffer(new(std::nothrow) FunctionData[kMaxFunctionEntries]),
	fStatus(B_OK)
{
	B_INITIALIZE_SPINLOCK(&fFunctionLock);

	if (fFunctionData == NULL || fSortBuffer == NULL) {
		fStatus = B_NO_MEMORY;
		return;
	}
	memset(fFunctionData, 0, sizeof(FunctionData) * kMaxFunctionEntries);

	memset(fFunctionStacks, 0, sizeof(fFunctionStacks));

	for (int32 i = 0; i < SMP_MAX_CPUS; i++) {
		fFunctionStacks[i]
			= new(std::nothrow) FunctionEntry[kMaxFunctionStackEntries];
		if (fFunctionStacks[i] == NULL) {
			fStatus = B_NO_MEMORY;
			delete[] fFunctionData;
			fFunctionData = NULL;
			for (int32 j = 0; j < i; j++) {
				delete[] fFunctionStacks[j];
				fFunctionStacks[j] = NULL;
			}
			return;
		}
		memset(fFunctionStacks[i], 0,
			sizeof(FunctionEntry) * kMaxFunctionStackEntries);
	}
	memset(fFunctionStackPointers, 0, sizeof(int32) * SMP_MAX_CPUS);
}


Profiler::~Profiler()
{
	for (int32 i = 0; i < SMP_MAX_CPUS; i++)
		delete[] fFunctionStacks[i];
	delete[] fFunctionData;
	delete[] fSortBuffer;
}


bool
Profiler::EnterFunction(int32 cpu, const char* functionName)
{
	InterruptsLocker _;
	if (fStatus != B_OK)
		return false;

	nanotime_t start = system_time_nsecs();

	FunctionData* function = _FindFunction(functionName);
	if (function == NULL)
		return false;
	atomic_add(&function->fCalled, 1);

	int32 stackDepth = fFunctionStackPointers[cpu];
	if (stackDepth >= (int32)kMaxFunctionStackEntries)
		return false;

	fFunctionStackPointers[cpu]++;
	FunctionEntry* stackEntry = &fFunctionStacks[cpu][stackDepth];

	stackEntry->fFunction = function;
	stackEntry->fEntryTime = start;
	stackEntry->fOthersTime = 0;

	nanotime_t stop = system_time_nsecs();
	stackEntry->fProfilerTime = stop - start;

	return true;
}


void
Profiler::ExitFunction(int32 cpu, const char* functionName)
{
	InterruptsLocker _;
	if (fStatus != B_OK)
		return;

	nanotime_t start = system_time_nsecs();

	int32 stackDepth = fFunctionStackPointers[cpu];
	// If EnterFunction failed due to stack overflow, it returned early.
	// In that case, we should not decrement the pointer.
	// The RAII Function object tracks whether EnterFunction succeeded,
	// so ExitFunction should only be called if it did.
	// However, we still check for underflow to catch manual misuse.
	if (stackDepth <= 0)
		return;

	stackDepth = --fFunctionStackPointers[cpu];
	if (stackDepth >= (int32)kMaxFunctionStackEntries)
		return;

	FunctionEntry* stackEntry = &fFunctionStacks[cpu][stackDepth];

	nanotime_t timeSpent = start - stackEntry->fEntryTime;
	timeSpent -= stackEntry->fProfilerTime;

	atomic_add64(&stackEntry->fFunction->fTimeInclusive, timeSpent / 1000);
	atomic_add64(&stackEntry->fFunction->fTimeExclusive,
		(timeSpent - stackEntry->fOthersTime) / 1000);

	nanotime_t profilerTime = stackEntry->fProfilerTime;
	if (stackDepth > 0) {
		stackEntry = &fFunctionStacks[cpu][stackDepth - 1];
		stackEntry->fOthersTime += timeSpent;
		stackEntry->fProfilerTime += profilerTime;

		nanotime_t stop = system_time_nsecs();
		stackEntry->fProfilerTime += stop - start;
	}
}


void
Profiler::DumpCalled(uint32 maxCount)
{
	uint32 count = _FunctionCount();
	memcpy(fSortBuffer, fFunctionData, count * sizeof(FunctionData));

	qsort(fSortBuffer, count, sizeof(FunctionData),
		&_CompareFunctions<int32, &FunctionData::fCalled>);

	if (maxCount > 0)
		count = std::min(count, maxCount);
	_Dump(fSortBuffer, count);
}


void
Profiler::DumpTimeInclusive(uint32 maxCount)
{
	uint32 count = _FunctionCount();
	memcpy(fSortBuffer, fFunctionData, count * sizeof(FunctionData));

	qsort(fSortBuffer, count, sizeof(FunctionData),
		&_CompareFunctions<nanotime_t, &FunctionData::fTimeInclusive>);

	if (maxCount > 0)
		count = std::min(count, maxCount);
	_Dump(fSortBuffer, count);
}


void
Profiler::DumpTimeExclusive(uint32 maxCount)
{
	uint32 count = _FunctionCount();
	memcpy(fSortBuffer, fFunctionData, count * sizeof(FunctionData));

	qsort(fSortBuffer, count, sizeof(FunctionData),
		&_CompareFunctions<nanotime_t, &FunctionData::fTimeExclusive>);

	if (maxCount > 0)
		count = std::min(count, maxCount);
	_Dump(fSortBuffer, count);
}


void
Profiler::DumpTimeInclusivePerCall(uint32 maxCount)
{
	uint32 count = _FunctionCount();
	memcpy(fSortBuffer, fFunctionData, count * sizeof(FunctionData));

	qsort(fSortBuffer, count, sizeof(FunctionData),
		&_CompareFunctionsPerCall<nanotime_t, &FunctionData::fTimeInclusive>);

	if (maxCount > 0)
		count = std::min(count, maxCount);
	_Dump(fSortBuffer, count);
}


void
Profiler::DumpTimeExclusivePerCall(uint32 maxCount)
{
	uint32 count = _FunctionCount();
	memcpy(fSortBuffer, fFunctionData, count * sizeof(FunctionData));

	qsort(fSortBuffer, count, sizeof(FunctionData),
		&_CompareFunctionsPerCall<nanotime_t, &FunctionData::fTimeExclusive>);

	if (maxCount > 0)
		count = std::min(count, maxCount);
	_Dump(fSortBuffer, count);
}


/* static */ Profiler*
Profiler::Get()
{
	return sProfiler;
}


/* static */ void
Profiler::Initialize()
{
	sProfiler = new(std::nothrow) Profiler;
	if (sProfiler == NULL || sProfiler->GetStatus() != B_OK)
		panic("Scheduler::Profiling::Profiler: could not initialize profiler");

	add_debugger_command_etc("scheduler_profiler", &dump_profiler,
		"Show data collected by scheduler profiler",
		"[ <field> [ <count> ] ]\n"
		"Shows data collected by scheduler profiler\n"
		"  <field>   - Field used to sort functions. Available: called,"
			" time-inclusive, time-inclusive-per-call, time-exclusive,"
			" time-exclusive-per-call.\n"
		"              (defaults to \"called\")\n"
		"  <count>   - Maximum number of showed functions.\n", 0);
}


uint32
Profiler::_FunctionCount() const
{
	uint32 count;
	for (count = 0; count < kMaxFunctionEntries; count++) {
		if (fFunctionData[count].fFunction == NULL)
			break;
	}
	return count;
}


void
Profiler::_Dump(FunctionData* data, uint32 count)
{
	kprintf("Function calls (%" B_PRId32 " functions):\n", count);
	kprintf("    called time-inclusive per-call time-exclusive per-call "
		"function\n");
	for (uint32 i = 0; i < count; i++) {
		FunctionData* function = &data[i];
		kprintf("%10" B_PRId32 " %14" B_PRId64 " %8" B_PRId64 " %14" B_PRId64
			" %8" B_PRId64 " %s\n", function->fCalled,
			function->fTimeInclusive,
			function->fCalled > 0 ? function->fTimeInclusive / function->fCalled
				: 0,
			function->fTimeExclusive,
			function->fCalled > 0 ? function->fTimeExclusive / function->fCalled
				: 0,
			function->fFunction);
	}
}


Profiler::FunctionData*
Profiler::_FindFunction(const char* function)
{
	// Single locked scan: the unlocked pre-scan provided no correctness
	// benefit (writes to fFunctionData need the lock anyway) and scanned
	// up to kMaxFunctionEntries (1024) entries on every miss.  A single
	// locked pass is simpler and equally fast since fFunctionLock is
	// uncontended in normal operation.
	InterruptsSpinLocker _(fFunctionLock);
	for (uint32 i = 0; i < kMaxFunctionEntries; i++) {
		if (fFunctionData[i].fFunction == NULL) {
			fFunctionData[i].fFunction = function;
			return fFunctionData + i;
		}
		if (!strcmp(fFunctionData[i].fFunction, function))
			return fFunctionData + i;
	}

	return NULL;
}


template<typename Type, Type Profiler::FunctionData::*Member>
/* static */ int
Profiler::_CompareFunctions(const void* _a, const void* _b)
{
	const FunctionData* a = static_cast<const FunctionData*>(_a);
	const FunctionData* b = static_cast<const FunctionData*>(_b);

	if (b->*Member > a->*Member)
		return 1;
	if (b->*Member < a->*Member)
		return -1;
	return 0;
}


template<typename Type, Type Profiler::FunctionData::*Member>
/* static */ int
Profiler::_CompareFunctionsPerCall(const void* _a, const void* _b)
{
	const FunctionData* a = static_cast<const FunctionData*>(_a);
	const FunctionData* b = static_cast<const FunctionData*>(_b);

	Type valueA = a->fCalled > 0 ? a->*Member / a->fCalled : 0;
	Type valueB = b->fCalled > 0 ? b->*Member / b->fCalled : 0;

	if (valueB > valueA)
		return 1;
	if (valueB < valueA)
		return -1;
	return 0;
}


static int
dump_profiler(int argc, char** argv)
{
	if (argc < 2) {
		Profiler::Get()->DumpCalled(0);
		return 0;
	}

	int32 count = 0;
	if (argc >= 3)
		count = parse_expression(argv[2]);
	count = std::max(count, int32(0));

	if (!strcmp(argv[1], "called"))
		Profiler::Get()->DumpCalled(count);
	else if (!strcmp(argv[1], "time-inclusive"))
		Profiler::Get()->DumpTimeInclusive(count);
	else if (!strcmp(argv[1], "time-inclusive-per-call"))
		Profiler::Get()->DumpTimeInclusivePerCall(count);
	else if (!strcmp(argv[1], "time-exclusive"))
		Profiler::Get()->DumpTimeExclusive(count);
	else if (!strcmp(argv[1], "time-exclusive-per-call"))
		Profiler::Get()->DumpTimeExclusivePerCall(count);
	else
		print_debugger_command_usage(argv[0]);

	return 0;
}


#endif	// SCHEDULER_PROFILING
