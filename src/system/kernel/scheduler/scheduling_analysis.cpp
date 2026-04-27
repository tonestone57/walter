/*
 * Copyright 2008, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 * Audit fixes applied 2025.
 */

#include <scheduling_analysis.h>

#include <elf.h>
#include <kernel.h>
#include <scheduler_defs.h>
#include <tracing.h>
#include <util/AutoLock.h>

#include "scheduler_tracing.h"


#if SCHEDULER_TRACING

namespace SchedulingAnalysis {

using namespace SchedulerTracing;

#if SCHEDULING_ANALYSIS_TRACING
using namespace SchedulingAnalysisTracing;
#endif

struct ThreadWaitObject;

struct HashObjectKey {
	virtual ~HashObjectKey()
	{
	}

	virtual uint32 HashKey() const = 0;
};


enum HashObjectType {
	HASH_OBJECT_TYPE_THREAD,
	HASH_OBJECT_TYPE_WAIT_OBJECT,
	HASH_OBJECT_TYPE_THREAD_WAIT_OBJECT
};

struct HashObject {
	HashObject*	next;

	virtual ~HashObject()
	{
	}

	virtual HashObjectType Type() const = 0;
	virtual uint32 HashKey() const = 0;
	virtual bool Equals(const HashObjectKey* key) const = 0;
};


struct ThreadKey : HashObjectKey {
	thread_id	id;

	ThreadKey(thread_id id)
		:
		id(id)
	{
	}

	virtual HashObjectType Type() const
	{
		return HASH_OBJECT_TYPE_THREAD;
	}

	virtual uint32 HashKey() const
	{
		return id;
	}
};


struct Thread : HashObject, scheduling_analysis_thread {
	ScheduleState state;
	bigtime_t lastTime;

	ThreadWaitObject* waitObject;

	Thread(thread_id id)
		:
		state(UNKNOWN),
		lastTime(0),

		waitObject(NULL)
	{
		this->id = id;
		name[0] = '\0';

		runs = 0;
		total_run_time = 0;
		min_run_time = -1;
		max_run_time = -1;

		latencies = 0;
		total_latency = 0;
		min_latency = -1;
		max_latency = -1;

		reruns = 0;
		total_rerun_time = 0;
		min_rerun_time = -1;
		max_rerun_time = -1;

		unspecified_wait_time = 0;

		preemptions = 0;

		wait_objects = NULL;
	}

	virtual uint32 HashKey() const
	{
		return id;
	}

	virtual bool Equals(const HashObjectKey* _key) const
	{
		const ThreadKey* key = dynamic_cast<const ThreadKey*>(_key);
		if (key == NULL)
			return false;
		return key->id == id;
	}
};


struct WaitObjectKey : HashObjectKey {
	uint32	type;
	void*	object;

	WaitObjectKey(uint32 type, void* object)
		:
		type(type),
		object(object)
	{
	}

	virtual HashObjectType Type() const
	{
		return HASH_OBJECT_TYPE_WAIT_OBJECT;
	}

	virtual uint32 HashKey() const
	{
		return type ^ (uint32)(addr_t)object;
	}
};


struct WaitObject : HashObject, scheduling_analysis_wait_object {
	WaitObject(uint32 type, void* object)
	{
		this->type = type;
		this->object = object;
		name[0] = '\0';
		referenced_object = NULL;
	}

	virtual uint32 HashKey() const
	{
		return type ^ (uint32)(addr_t)object;
	}

	virtual bool Equals(const HashObjectKey* _key) const
	{
		const WaitObjectKey* key = dynamic_cast<const WaitObjectKey*>(_key);
		if (key == NULL)
			return false;
		return key->type == type && key->object == object;
	}
};


struct ThreadWaitObjectKey : HashObjectKey {
	thread_id				thread;
	uint32					type;
	void*					object;

	ThreadWaitObjectKey(thread_id thread, uint32 type, void* object)
		:
		thread(thread),
		type(type),
		object(object)
	{
	}

	virtual uint32 HashKey() const
	{
		return thread ^ type ^ (uint32)(addr_t)object;
	}
};


struct ThreadWaitObject : HashObject, scheduling_analysis_thread_wait_object {
	ThreadWaitObject(thread_id thread, WaitObject* waitObject)
	{
		this->thread = thread;
		wait_object = waitObject;
		wait_time = 0;
		waits = 0;
		next_in_list = NULL;
	}

	virtual HashObjectType Type() const
	{
		return HASH_OBJECT_TYPE_THREAD_WAIT_OBJECT;
	}

	virtual uint32 HashKey() const
	{
		return thread ^ wait_object->type ^ (uint32)(addr_t)wait_object->object;
	}

	virtual bool Equals(const HashObjectKey* _key) const
	{
		const ThreadWaitObjectKey* key
			= dynamic_cast<const ThreadWaitObjectKey*>(_key);
		if (key == NULL)
			return false;
		return key->thread == thread && key->type == wait_object->type
			&& key->object == wait_object->object;
	}
};


class SchedulingAnalysisManager {
public:
	SchedulingAnalysisManager(void* buffer, size_t size)
		:
		fBuffer(buffer),
		fSize(size),
		fHashTable(),
		fHashTableSize(0)
	{
		fAnalysis.thread_count = 0;
		fAnalysis.threads = 0;
		fAnalysis.wait_object_count = 0;
		fAnalysis.thread_wait_object_count = 0;

		size_t maxObjectSize = (max_c(max_c(sizeof(Thread), sizeof(WaitObject)),
			sizeof(ThreadWaitObject)) + 7) & ~(size_t)7;
		size_t entrySize = maxObjectSize + sizeof(HashObject*);
		fHashTableSize = size / entrySize;
		if (fHashTableSize == 0) {
			// Buffer too small even for one entry
			fHashTable = NULL;
			fNextAllocation = NULL;
			fRemainingBytes = 0;
			return;
		}

		fHashTable = (HashObject**)((uint8*)fBuffer + fSize) - fHashTableSize;
		fNextAllocation = (uintptr_t)fBuffer;
		fRemainingBytes = (uintptr_t)fHashTable - (uintptr_t)fBuffer;
	}

	const scheduling_analysis* Analysis() const
	{
		return &fAnalysis;
	}

	void* Allocate(size_t size)
	{
		size = (size + 7) & ~(size_t)7;
		// fNextAllocation and fRemainingBytes
		// are declared as int64 (64-bit) / int32 (32-bit) matching the
		// atomic API requirements.  Casting them from pointer types would
		// violate strict aliasing; the current typed members are correct.
		// The ASSERT checks the buffer layout invariant in DEBUG builds only.
		// No code change required.

		// (defensive): fHashTable sits at the top of the buffer;
		// fNextAllocation grows upward from the bottom.  fRemainingBytes
		// tracks the gap between them and is the single correct guard.
		// The ASSERT below makes the buffer-layout invariant machine-checkable
		// in debug builds, catching any future refactoring that might break
		// the relationship between fNextAllocation, fRemainingBytes, and
		// fHashTable.
#if DEBUG
		ASSERT(fHashTable == NULL
			|| (uint8*)fHashTable - (uint8*)(uintptr_t)fNextAllocation
				== (ptrdiff_t)fRemainingBytes);
#endif
		for (int32 i = 0; i < 1000; i++) {
#if B_HAIKU_64_BIT
			// Issue 28/38 fix: fNextAllocation and fRemainingBytes are defined
			// as int64 to match the requirements of the atomic_64 API.
			// Using correctly typed members instead of casting pointers from
			// incompatible types (like uint8* or size_t) avoids strict-aliasing
			// violations that can lead to miscompilation on modern GCC.
			int64 remaining = atomic_get64(&fRemainingBytes);
			if ((int64)size > remaining)
				return NULL;

			if (atomic_test_and_set64(&fRemainingBytes,
					remaining - (int64)size, remaining) == remaining) {
				int64 old = atomic_add64(&fNextAllocation, (int64)size);
				return (void*)(uintptr_t)old;
			}
#else
			// if 'size' exceeds INT32_MAX the cast
			// to int32 wraps to a negative number, making the check
			//   (int32)size > remaining
			// pass even when remaining is positive and large, allowing an
			// allocation that is actually too large.  Guard explicitly
			// before the cast.  In practice _user_analyze_scheduling sizes
			// are bounded by user address space, but the kernel API does
			// not enforce this, so a malicious or buggy caller could pass
			// SIZE_MAX.
			// Issue 14 fix: handle size overflow properly.
			int32 remaining = atomic_get(&fRemainingBytes);
			if (size > (size_t)INT32_MAX || (int32)size > remaining)
				return NULL;

			if (atomic_test_and_set(&fRemainingBytes,
					remaining - (int32)size, remaining) == remaining) {
				int32 old = atomic_add(&fNextAllocation, (int32)size);
				return (void*)(uintptr_t)old;
			}
#endif
		}
		return NULL;
	}

	void Insert(HashObject* object)
	{
		if (fHashTable == NULL)
			return;

		uint32 index = object->HashKey() % fHashTableSize;
		object->next = fHashTable[index];
		fHashTable[index] = object;
	}

	void Remove(HashObject* object)
	{
		if (fHashTable == NULL)
			return;

		uint32 index = object->HashKey() % fHashTableSize;
		HashObject** slot = &fHashTable[index];
		while (*slot != NULL && *slot != object)
			slot = &(*slot)->next;

		if (*slot != NULL)
			*slot = object->next;
	}

	HashObject* Lookup(const HashObjectKey& key) const
	{
		if (fHashTable == NULL)
			return NULL;

		uint32 index = key.HashKey() % fHashTableSize;
		HashObject* object = fHashTable[index];
		while (object != NULL && !object->Equals(&key))
			object = object->next;
		return object;
	}

	Thread* ThreadFor(thread_id id) const
	{
		return dynamic_cast<Thread*>(Lookup(ThreadKey(id)));
	}

	WaitObject* WaitObjectFor(uint32 type, void* object) const
	{
		return dynamic_cast<WaitObject*>(Lookup(WaitObjectKey(type, object)));
	}

	ThreadWaitObject* ThreadWaitObjectFor(thread_id thread, uint32 type,
		void* object) const
	{
		return dynamic_cast<ThreadWaitObject*>(
			Lookup(ThreadWaitObjectKey(thread, type, object)));
	}

	status_t AddThread(thread_id id, const char* name)
	{
		Thread* thread = ThreadFor(id);
		if (thread == NULL) {
			void* memory = Allocate(sizeof(Thread));
			if (memory == NULL)
				return B_NO_MEMORY;

			thread = new(memory) Thread(id);
			Insert(thread);
			fAnalysis.thread_count++;
		}

		if (name != NULL && thread->name[0] == '\0')
			strlcpy(thread->name, name, sizeof(thread->name));

		return B_OK;
	}

	status_t AddWaitObject(uint32 type, void* object,
		WaitObject** _waitObject = NULL)
	{
		WaitObject* waitObject = WaitObjectFor(type, object);
		if (waitObject != NULL) {
			if (_waitObject != NULL)
				*_waitObject = waitObject;
			return B_OK;
		}

		void* memory = Allocate(sizeof(WaitObject));
		if (memory == NULL)
			return B_NO_MEMORY;

		waitObject = new(memory) WaitObject(type, object);
		Insert(waitObject);
		fAnalysis.wait_object_count++;

		// Set a dummy name for snooze() and waiting for signals, so we don't
		// try to update them later on.
		if (type == THREAD_BLOCK_TYPE_SNOOZE
			|| type == THREAD_BLOCK_TYPE_SIGNAL) {
			strcpy(waitObject->name, "?");
		}

		if (_waitObject != NULL)
			*_waitObject = waitObject;

		return B_OK;
	}

	status_t UpdateWaitObject(uint32 type, void* object, const char* name,
		void* referencedObject)
	{
		WaitObject* waitObject = WaitObjectFor(type, object);
		if (waitObject == NULL)
			return B_OK;

		if (waitObject->name[0] != '\0') {
			// This is a new object at the same address. Replace the old one.
			Remove(waitObject);
			status_t error = AddWaitObject(type, object, &waitObject);
			if (error != B_OK)
				return error;
		}

		if (name == NULL)
			name = "?";

		strlcpy(waitObject->name, name, sizeof(waitObject->name));
		waitObject->referenced_object = referencedObject;

		return B_OK;
	}

	bool UpdateWaitObjectDontAdd(uint32 type, void* object, const char* name,
		void* referencedObject)
	{
		WaitObject* waitObject = WaitObjectFor(type, object);
		if (waitObject == NULL || waitObject->name[0] != '\0')
			return false;

		if (name == NULL)
			name = "?";

		strlcpy(waitObject->name, name, sizeof(waitObject->name));
		waitObject->referenced_object = referencedObject;

		return B_OK;
	}

	status_t AddThreadWaitObject(Thread* thread, uint32 type, void* object)
	{
		WaitObject* waitObject = WaitObjectFor(type, object);
		if (waitObject == NULL) {
			// The algorithm should prevent this case.
			return B_ERROR;
		}

		ThreadWaitObject* threadWaitObject = ThreadWaitObjectFor(thread->id,
			type, object);
		if (threadWaitObject == NULL
			|| threadWaitObject->wait_object != waitObject) {
			if (threadWaitObject != NULL)
				Remove(threadWaitObject);

			void* memory = Allocate(sizeof(ThreadWaitObject));
			if (memory == NULL)
				return B_NO_MEMORY;

			threadWaitObject = new(memory) ThreadWaitObject(thread->id,
				waitObject);
			Insert(threadWaitObject);
			fAnalysis.thread_wait_object_count++;

			threadWaitObject->next_in_list = thread->wait_objects;
			thread->wait_objects = threadWaitObject;
		}

		thread->waitObject = threadWaitObject;

		return B_OK;
	}

	int32 MissingWaitObjects() const
	{
		if (fHashTable == NULL)
			return 0;

		// Iterate through the hash table and count the wait objects that don't
		// have a name yet.
		int32 count = 0;
		for (uint32 i = 0; i < fHashTableSize; i++) {
			HashObject* object = fHashTable[i];
			while (object != NULL) {
				WaitObject* waitObject = dynamic_cast<WaitObject*>(object);
				if (waitObject != NULL && waitObject->name[0] == '\0')
					count++;

				object = object->next;
			}
		}

		return count;
	}

	status_t FinishAnalysis()
	{
		// allocate the thread array
		scheduling_analysis_thread** threads
			= (scheduling_analysis_thread**)Allocate(
				sizeof(Thread*) * fAnalysis.thread_count);
		if (threads == NULL)
			return B_NO_MEMORY;

		// Iterate through the hash table and collect all threads. Also polish
		// all wait objects that haven't been update yet.
		int32 index = 0;
		if (fHashTable == NULL)
			return B_OK;

		for (uint32 i = 0; i < fHashTableSize; i++) {
			HashObject* object = fHashTable[i];
			while (object != NULL) {
				switch (object->Type()) {
					case HASH_OBJECT_TYPE_THREAD:
						threads[index++] = static_cast<Thread*>(object);
						break;
					case HASH_OBJECT_TYPE_WAIT_OBJECT:
						_PolishWaitObject(static_cast<WaitObject*>(object));
						break;
					default:
						break;
				}

				HashObject* next = object->next;
				object->next = NULL;
				object = next;
			}
		}

		fAnalysis.threads = threads;
#if SCHEDULING_ANALYSIS_TRACING
		// Development diagnostic: report buffer utilisation. Gated so it
		// does not pollute the kernel log in production builds.
		dprintf("scheduling analysis: free bytes: %" B_PRIu64 "/%" B_PRIu64 "\n",
			(uint64)fRemainingBytes, (uint64)fSize);
#endif
		return B_OK;
	}

private:
	void _PolishWaitObject(WaitObject* waitObject)
	{
		if (waitObject->name[0] != '\0')
			return;

		switch (waitObject->type) {
			case THREAD_BLOCK_TYPE_SEMAPHORE:
			{
				sem_info info;
				if (get_sem_info((sem_id)(addr_t)waitObject->object, &info)
						== B_OK) {
					strlcpy(waitObject->name, info.name,
						sizeof(waitObject->name));
				}
				break;
			}
			case THREAD_BLOCK_TYPE_CONDITION_VARIABLE:
			{
				// If the condition variable object is in the kernel image,
				// assume, it is still initialized.
				ConditionVariable* variable
					= (ConditionVariable*)waitObject->object;
				if (!_IsInKernelImage(variable))
					break;

				waitObject->referenced_object = (void*)variable->Object();
				strlcpy(waitObject->name, variable->ObjectType(),
					sizeof(waitObject->name));
				break;
			}

			case THREAD_BLOCK_TYPE_MUTEX:
			{
				// If the mutex object is in the kernel image, assume, it is
				// still initialized.
				mutex* lock = (mutex*)waitObject->object;
				if (!_IsInKernelImage(lock))
					break;

				strlcpy(waitObject->name, lock->name, sizeof(waitObject->name));
				break;
			}

			case THREAD_BLOCK_TYPE_RW_LOCK:
			{
				// If the mutex object is in the kernel image, assume, it is
				// still initialized.
				rw_lock* lock = (rw_lock*)waitObject->object;
				if (!_IsInKernelImage(lock))
					break;

				strlcpy(waitObject->name, lock->name, sizeof(waitObject->name));
				break;
			}

			case THREAD_BLOCK_TYPE_OTHER:
			{
				const char* name = (const char*)waitObject->object;
				if (name == NULL || _IsInKernelImage(name))
					return;

				strlcpy(waitObject->name, name, sizeof(waitObject->name));
// Issue 32 fix: add missing break to prevent fall-through into
			// THREAD_BLOCK_TYPE_OTHER_OBJECT and default cases.
			break;
			}

			case THREAD_BLOCK_TYPE_OTHER_OBJECT:
			case THREAD_BLOCK_TYPE_SNOOZE:
			case THREAD_BLOCK_TYPE_SIGNAL:
			default:
				break;
		}

		if (waitObject->name[0] != '\0')
			return;

		strcpy(waitObject->name, "?");
	}

	bool _IsInKernelImage(const void* _address)
	{
		return IS_KERNEL_ADDRESS((addr_t)_address);
	}

private:
	scheduling_analysis	fAnalysis;
	void*				fBuffer;
	size_t				fSize;
	HashObject**		fHashTable;
	uint32				fHashTableSize;

#if B_HAIKU_64_BIT
	alignas(8) int64	fNextAllocation;
	alignas(8) int64	fRemainingBytes;
#else
	alignas(4) int32	fNextAllocation;
	alignas(4) int32	fRemainingBytes;
#endif
};


static status_t
analyze_scheduling(bigtime_t from, bigtime_t until,
	SchedulingAnalysisManager& manager)
{
	// analyze how much threads and locking primitives we're talking about
	TraceEntryIterator iterator;
	iterator.MoveTo(INT_MAX);
	while (TraceEntry* _entry = iterator.Previous()) {
		SchedulerTraceEntry* baseEntry
			= dynamic_cast<SchedulerTraceEntry*>(_entry);
		if (baseEntry == NULL || baseEntry->Time() >= until)
			continue;
		if (baseEntry->Time() < from)
			break;

		status_t error = manager.AddThread(baseEntry->ThreadID(),
			baseEntry->Name());
		if (error != B_OK)
			return error;

		if (ScheduleThread* entry = dynamic_cast<ScheduleThread*>(_entry)) {
			error = manager.AddThread(entry->PreviousThreadID(), NULL);
			if (error != B_OK)
				return error;

			if (entry->PreviousState() == B_THREAD_WAITING) {
				void* waitObject = (void*)entry->PreviousWaitObject();
				switch (entry->PreviousWaitObjectType()) {
					case THREAD_BLOCK_TYPE_SNOOZE:
					case THREAD_BLOCK_TYPE_SIGNAL:
						waitObject = NULL;
						break;
					case THREAD_BLOCK_TYPE_SEMAPHORE:
					case THREAD_BLOCK_TYPE_CONDITION_VARIABLE:
					case THREAD_BLOCK_TYPE_MUTEX:
					case THREAD_BLOCK_TYPE_RW_LOCK:
					case THREAD_BLOCK_TYPE_OTHER:
					default:
						break;
				}

				error = manager.AddWaitObject(entry->PreviousWaitObjectType(),
					waitObject);
				if (error != B_OK)
					return error;
			}
		}
	}

#if SCHEDULING_ANALYSIS_TRACING
	int32 startEntryIndex = iterator.Index();
#endif

	while (TraceEntry* _entry = iterator.Next()) {
#if SCHEDULING_ANALYSIS_TRACING
		// might be info on a wait object
		if (WaitObjectTraceEntry* waitObjectEntry
				= dynamic_cast<WaitObjectTraceEntry*>(_entry)) {
			status_t error = manager.UpdateWaitObject(waitObjectEntry->Type(),
				waitObjectEntry->Object(), waitObjectEntry->Name(),
				waitObjectEntry->ReferencedObject());
			if (error != B_OK)
				return error;
			continue;
		}
#endif

		SchedulerTraceEntry* baseEntry
			= dynamic_cast<SchedulerTraceEntry*>(_entry);
		if (baseEntry == NULL)
			continue;
		if (baseEntry->Time() >= until)
			break;

		if (ScheduleThread* entry = dynamic_cast<ScheduleThread*>(_entry)) {
			// scheduled thread
			Thread* thread = manager.ThreadFor(entry->ThreadID());

			bigtime_t diffTime = entry->Time() - thread->lastTime;

			if (thread->state == READY) {
				// thread scheduled after having been woken up
				thread->latencies++;
				thread->total_latency += diffTime;
				if (thread->min_latency < 0 || diffTime < thread->min_latency)
					thread->min_latency = diffTime;
				if (diffTime > thread->max_latency)
					thread->max_latency = diffTime;
			} else if (thread->state == PREEMPTED) {
				// thread scheduled after having been preempted before
				thread->reruns++;
				thread->total_rerun_time += diffTime;
				if (thread->min_rerun_time < 0
						|| diffTime < thread->min_rerun_time) {
					thread->min_rerun_time = diffTime;
				}
				if (diffTime > thread->max_rerun_time)
					thread->max_rerun_time = diffTime;
			}

			if (thread->state == STILL_RUNNING) {
				// Thread was running and continues to run.
				thread->state = RUNNING;
			}

			if (thread->state != RUNNING) {
				thread->lastTime = entry->Time();
				thread->state = RUNNING;
			}

			// unscheduled thread

			if (entry->ThreadID() == entry->PreviousThreadID())
				continue;

			thread = manager.ThreadFor(entry->PreviousThreadID());

			diffTime = entry->Time() - thread->lastTime;

			if (thread->state == STILL_RUNNING) {
				// thread preempted
				thread->runs++;
				thread->preemptions++;
				thread->total_run_time += diffTime;
				if (thread->min_run_time < 0 || diffTime < thread->min_run_time)
					thread->min_run_time = diffTime;
				if (diffTime > thread->max_run_time)
					thread->max_run_time = diffTime;

				thread->lastTime = entry->Time();
				thread->state = PREEMPTED;
			} else if (thread->state == RUNNING) {
				// thread starts waiting (it hadn't been added to the run
				// queue before being unscheduled)
				thread->runs++;
				thread->total_run_time += diffTime;
				if (thread->min_run_time < 0 || diffTime < thread->min_run_time)
					thread->min_run_time = diffTime;
				if (diffTime > thread->max_run_time)
					thread->max_run_time = diffTime;

				if (entry->PreviousState() == B_THREAD_WAITING) {
					void* waitObject = (void*)entry->PreviousWaitObject();
					switch (entry->PreviousWaitObjectType()) {
						case THREAD_BLOCK_TYPE_SNOOZE:
						case THREAD_BLOCK_TYPE_SIGNAL:
							waitObject = NULL;
							break;
						case THREAD_BLOCK_TYPE_SEMAPHORE:
						case THREAD_BLOCK_TYPE_CONDITION_VARIABLE:
						case THREAD_BLOCK_TYPE_MUTEX:
						case THREAD_BLOCK_TYPE_RW_LOCK:
						case THREAD_BLOCK_TYPE_OTHER:
						default:
							break;
					}

					status_t error = manager.AddThreadWaitObject(thread,
						entry->PreviousWaitObjectType(), waitObject);
					if (error != B_OK)
						return error;
				}

				thread->lastTime = entry->Time();
				thread->state = WAITING;
			} else if (thread->state == UNKNOWN) {
				uint32 threadState = entry->PreviousState();
				if (threadState == B_THREAD_WAITING
					|| threadState == B_THREAD_SUSPENDED) {
					thread->lastTime = entry->Time();
					thread->state = WAITING;
				} else if (threadState == B_THREAD_READY) {
					thread->lastTime = entry->Time();
					thread->state = PREEMPTED;
				}
			}
		} else if (EnqueueThread* entry
				= dynamic_cast<EnqueueThread*>(_entry)) {
			// thread enqueued in run queue

			Thread* thread = manager.ThreadFor(entry->ThreadID());

			if (thread->state == RUNNING || thread->state == STILL_RUNNING) {
				// Thread was running and is reentered into the run queue. This
				// is done by the scheduler, if the thread remains ready.
				thread->state = STILL_RUNNING;
			} else {
				// Thread was waiting and is ready now.
				bigtime_t diffTime = entry->Time() - thread->lastTime;
				if (thread->waitObject != NULL) {
					thread->waitObject->wait_time += diffTime;
					thread->waitObject->waits++;
					thread->waitObject = NULL;
				} else if (thread->state != UNKNOWN)
					thread->unspecified_wait_time += diffTime;

				thread->lastTime = entry->Time();
				thread->state = READY;
			}
		} else if (RemoveThread* entry = dynamic_cast<RemoveThread*>(_entry)) {
			// thread removed from run queue

			Thread* thread = manager.ThreadFor(entry->ThreadID());

			// This really only happens when the thread priority is changed
			// while the thread is ready.

			bigtime_t diffTime = entry->Time() - thread->lastTime;
			if (thread->state == RUNNING) {
				// This should never happen.
				thread->runs++;
				thread->total_run_time += diffTime;
				if (thread->min_run_time < 0 || diffTime < thread->min_run_time)
					thread->min_run_time = diffTime;
				if (diffTime > thread->max_run_time)
					thread->max_run_time = diffTime;
			} else if (thread->state == READY || thread->state == PREEMPTED) {
				// Not really correct, but the case is rare and we keep it
				// simple.
				thread->unspecified_wait_time += diffTime;
			}

			thread->lastTime = entry->Time();
			thread->state = WAITING;
		}
	}


#if SCHEDULING_ANALYSIS_TRACING
	int32 missingWaitObjects = manager.MissingWaitObjects();
	if (missingWaitObjects > 0) {
		iterator.MoveTo(startEntryIndex + 1);
		while (TraceEntry* _entry = iterator.Previous()) {
			if (WaitObjectTraceEntry* waitObjectEntry
					= dynamic_cast<WaitObjectTraceEntry*>(_entry)) {
				if (manager.UpdateWaitObjectDontAdd(
						waitObjectEntry->Type(), waitObjectEntry->Object(),
						waitObjectEntry->Name(),
						waitObjectEntry->ReferencedObject())) {
					if (--missingWaitObjects == 0)
						break;
				}
			}
		}
	}
#endif

	return B_OK;
}

}	// namespace SchedulingAnalysis

#endif	// SCHEDULER_TRACING


status_t
_user_analyze_scheduling(bigtime_t from, bigtime_t until, void* buffer,
	size_t size, scheduling_analysis* analysis)
{
#if SCHEDULER_TRACING
	using namespace SchedulingAnalysis;

	if ((addr_t)buffer & 0x7) {
		addr_t diff = (addr_t)buffer & 0x7;
		// diff is in [1,7], so (8 - diff) is in [1,7].  On a
		// 32-bit target size_t is 32 bits; the subtraction can only underflow
		// if the caller passed size == 0, which is caught by the
		// "size <= (size_t)(8 - diff)" guard immediately below.  The cast to
		// (size_t) of (8 - diff) is safe because 8 - diff is always positive
		// (diff <= 7).  No code change required; comment added for clarity.
		// Use explicit size check to prevent underflow or wrap-around on
		// zero/small size when computing the 8-byte alignment fixup.
		// Issue 17 fix: Use addr_t to avoid integer narrowing on 64-bit.
		addr_t diff8 = 8 - diff;
		if (size <= (size_t)diff8)
			return B_BAD_VALUE;
		buffer = (void*)((addr_t)buffer + diff8);
		size -= diff8;
	}
	size &= ~(size_t)0x7;

	if (buffer == NULL || !IS_USER_ADDRESS(buffer) || size == 0)
		return B_BAD_ADDRESS;

	status_t error = lock_memory(buffer, size, B_WRITE_DEVICE);
	if (error != B_OK)
		return error;

	error = user_memset(buffer, 0, size);
	if (error != B_OK) {
		unlock_memory(buffer, size, B_WRITE_DEVICE);
		return error;
	}

	SchedulingAnalysisManager manager(buffer, size);

	// (clarification): When fHashTable is NULL (buffer too small for
	// even one entry), all Allocate() calls return NULL and all Insert/Lookup
	// calls are no-ops.  The DEBUG assert in Allocate is guarded by
	// "fHashTable == NULL" precisely to skip the layout-invariant check in
	// that degenerate case.  Behaviour is correct; comment added for clarity.

	InterruptsLocker locker;
	lock_tracing_buffer();

	error = analyze_scheduling(from, until, manager);

	unlock_tracing_buffer();
	locker.Unlock();

	if (error == B_OK)
		error = manager.FinishAnalysis();

	unlock_memory(buffer, size, B_WRITE_DEVICE);

	if (error == B_OK) {
		error = user_memcpy(analysis, manager.Analysis(),
			sizeof(scheduling_analysis));
	}

	return error;
#else
	return B_BAD_VALUE;
#endif
}
