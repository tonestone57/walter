/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_profiler.h"

#include <debug.h>
#include <KernelExport.h>
#include <util/atomic.h>
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
	memset(fHashTable, 0, sizeof(fHashTable));
	fNextFunctionSlot = 0;

	memset(fFunctionStacks, 0, sizeof(fFunctionStacks));

	for (int32 i = 0; i < SMP_MAX_CPUS; i++) {
		fFunctionStacks[i]
			= new(std::nothrow) FunctionEntry[kMaxFunctionStackEntries];
		if (fFunctionStacks[i] == NULL) {
			fStatus = B_NO_MEMORY;
			delete[] fFunctionData;
			delete[] fSortBuffer;
			fFunctionData = NULL;
			fSortBuffer = NULL;
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
	delete[] fFunctionData;
	delete[] fSortBuffer;

	for (int32 i = 0; i < SMP_MAX_CPUS; i++)
		delete[] fFunctionStacks[i];
}


bool
Profiler::EnterFunction(int32 cpu, const char* functionName)
{
	InterruptsLocker _;
	// (clarification): fFunctionStackPointers[cpu] is incremented
	// with a plain ++.  This is safe because:
	//   1. InterruptsLocker disables interrupts, preventing preemption on this
	//      CPU — no other thread on this CPU can enter concurrently.
	//   2. The 'cpu' argument equals smp_get_current_cpu(); two distinct CPUs
	//      always have different cpu_num values so they access different array
	//      slots.  No atomic operation is required.
	if (fStatus != B_OK)
		return false;

	nanotime_t start = system_time_nsecs();

	FunctionData* function = _FindFunction(functionName);
	if (function == NULL)
		return false;

	int32 stackDepth = fFunctionStackPointers[cpu];

	// Issue 75 fix: check stack depth BEFORE incrementing fCalled.
	// The original code incremented fCalled unconditionally, overstating
	// how many times the function was successfully profiled when the stack
	// was full and EnterFunction returned false.
	if (stackDepth >= (int32)kMaxFunctionStackEntries)
		return false;

	// Only count the call after we know we can successfully profile it.
	atomic_add(&function->fCalled, 1);
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
	uint32 hash = 0;
	for (const char* p = function; *p; p++)
		hash = (hash * 31 + *p);

	// Issue 25 fix: document the ABA safety argument for the lockless path.
	// FunctionData slots are allocated from fFunctionData[] and NEVER freed
	// or reused during the lifetime of the Profiler object (fNextFunctionSlot
	// only increases). Therefore a pointer read from fHashTable[index] via
	// atomic_pointer_get() cannot become dangling or be reused for a
	// different function while we dereference it — the "ABA problem" cannot
	// occur here. The lockless search is safe for the current design.
	//
	// WARNING: if function slot reclamation is ever added (e.g. to support
	// profiler reset), this lockless path MUST be protected by a read-side
	// hazard pointer or RCU mechanism before the slot can be freed.
	//
	// Lockless Search: fast path for existing entries.
	uint32 index = hash % kHashTableSize;
	uint32 startIndex = index;
	do {
		FunctionData* entry = atomic_pointer_get<FunctionData>(&fHashTable[index]);
		if (entry == NULL)
			break;

		memory_read_barrier();
		// Issue 20 fix: ensure pointed-to string data is visible.
		const char* entryFunction = entry->fFunction;
		memory_read_barrier();
		if (strcmp(entryFunction, function) == 0)
			return entry;

		index = (index + 1) % kHashTableSize;
	} while (index != startIndex);

	InterruptsSpinLocker _(fFunctionLock);

	// Double-checked Search: handle races and insertions.
	index = hash % kHashTableSize;
	startIndex = index;
	do {
		FunctionData* entry = atomic_pointer_get<FunctionData>(&fHashTable[index]);
		if (entry == NULL)
			break;

		memory_read_barrier();
		// Issue 20 fix: ensure pointed-to string data is visible.
		const char* entryFunction = entry->fFunction;
		memory_read_barrier();
		if (strcmp(entryFunction, function) == 0)
			return entry;

		index = (index + 1) % kHashTableSize;
	} while (index != startIndex);

	// Not in hash table, check if we have room for a new entry.
	if (fNextFunctionSlot < kMaxFunctionEntries) {
		FunctionData* entry = &fFunctionData[fNextFunctionSlot++];
		entry->fFunction = function;

		// Insert into hash table.
		index = hash % kHashTableSize;
		while (atomic_pointer_get<FunctionData>(&fHashTable[index]) != NULL)
			index = (index + 1) % kHashTableSize;

		memory_write_barrier();
		atomic_pointer_set<FunctionData>(&fHashTable[index], entry);

		return entry;
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
