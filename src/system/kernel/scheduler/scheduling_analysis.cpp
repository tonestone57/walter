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

enum HashObjectType {
	HASH_OBJECT_TYPE_THREAD,
	HASH_OBJECT_TYPE_WAIT_OBJECT,
	HASH_OBJECT_TYPE_THREAD_WAIT_OBJECT
};


struct HashObjectKey {
	virtual ~HashObjectKey()
	{
	}

	virtual HashObjectType Type() const = 0;
	virtual uint32 HashKey() const = 0;
	virtual bool Equals(const HashObjectKey* key) const = 0;
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
		return (uint32)id;
	}

	virtual bool Equals(const HashObjectKey* _key) const
	{
		if (_key->Type() != HASH_OBJECT_TYPE_THREAD)
			return false;
		return static_cast<const ThreadKey*>(_key)->id == id;
	}
};


struct Thread : HashObject, scheduling_analysis_thread {
	virtual HashObjectType Type() const
	{
		return HASH_OBJECT_TYPE_THREAD;
	}

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
		return (uint32)id;
	}

	virtual bool Equals(const HashObjectKey* _key) const
	{
		if (_key->Type() != HASH_OBJECT_TYPE_THREAD)
			return false;
		const ThreadKey* key = static_cast<const ThreadKey*>(_key);
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

	virtual bool Equals(const HashObjectKey* _key) const
	{
		if (_key->Type() != HASH_OBJECT_TYPE_WAIT_OBJECT)
			return false;
		const WaitObjectKey* key = static_cast<const WaitObjectKey*>(_key);
		return key->type == type && key->object == object;
	}
};


struct WaitObject : HashObject, scheduling_analysis_wait_object {
	virtual HashObjectType Type() const
	{
		return HASH_OBJECT_TYPE_WAIT_OBJECT;
	}

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
		if (_key->Type() != HASH_OBJECT_TYPE_WAIT_OBJECT)
			return false;
		const WaitObjectKey* key = static_cast<const WaitObjectKey*>(_key);
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

	virtual HashObjectType Type() const
	{
		return HASH_OBJECT_TYPE_THREAD_WAIT_OBJECT;
	}

	virtual uint32 HashKey() const
	{
		return (uint32)thread ^ type ^ (uint32)(addr_t)object;
	}

	virtual bool Equals(const HashObjectKey* _key) const
	{
		if (_key->Type() != HASH_OBJECT_TYPE_THREAD_WAIT_OBJECT)
			return false;
		const ThreadWaitObjectKey* key
			= static_cast<const ThreadWaitObjectKey*>(_key);
		return key->thread == thread && key->type == type && key->object == object;
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
		return (uint32)thread ^ wait_object->type ^ (uint32)(addr_t)wait_object->object;
	}

	virtual bool Equals(const HashObjectKey* _key) const
	{
		if (_key->Type() != HASH_OBJECT_TYPE_THREAD_WAIT_OBJECT)
			return false;
		const ThreadWaitObjectKey* key
			= static_cast<const ThreadWaitObjectKey*>(_key);
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
		fHashTable(NULL),
		fHashTableSize(0)
	{
		fAnalysis.thread_count = 0;
		fAnalysis.threads = NULL;
		fAnalysis.wait_object_count = 0;
		fAnalysis.thread_wait_object_count = 0;

		size_t maxObjectSize = (max_c(max_c(sizeof(Thread), sizeof(WaitObject)),
			sizeof(ThreadWaitObject)) + 7) & ~(size_t)7;
		size_t entrySize = maxObjectSize + sizeof(HashObject*);
		fHashTableSize = size / entrySize;
		if (fHashTableSize == 0) {
			fHashTable = NULL;
			fNextAllocation = (uintptr_t)fBuffer;
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
		for (int32 i = 0; i < 1000; i++) {
#if B_HAIKU_64_BIT
			int64 current = (int64)atomic_get64(reinterpret_cast<int64 volatile*>(&fNextAllocation));
			int64 newAlloc = current + (int64)size;
			int64 hashTableAddr = (int64)(uintptr_t)fHashTable;
			if (newAlloc > hashTableAddr)
				return NULL;
			if (atomic_test_and_set64(reinterpret_cast<int64 volatile*>(&fNextAllocation), newAlloc, current)
					== current) {
				atomic_add64(reinterpret_cast<int64 volatile*>(&fRemainingBytes), -(int64)size);
				return (void*)(uintptr_t)current;
			}
#else
			int32 current32 = atomic_get(reinterpret_cast<int32 volatile*>(&fNextAllocation));
			int32 newAlloc32 = current32 + (int32)size;
			int32 hashTableAddr32 = (int32)(uintptr_t)fHashTable;
			if (size > (size_t)B_INT32_MAX || newAlloc32 > hashTableAddr32)
				return NULL;
			if (atomic_test_and_set(reinterpret_cast<int32 volatile*>(&fNextAllocation), newAlloc32, current32)
					== current32) {
				atomic_add(reinterpret_cast<int32 volatile*>(&fRemainingBytes), -(int32)size);
				return (void*)(uintptr_t)current32;
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
		HashObject* object = Lookup(ThreadKey(id));
		if (object == NULL || object->Type() != HASH_OBJECT_TYPE_THREAD)
			return NULL;
		return static_cast<Thread*>(object);
	}

	WaitObject* WaitObjectFor(uint32 type, void* object) const
	{
		HashObject* hashObject = Lookup(WaitObjectKey(type, object));
		if (hashObject == NULL || hashObject->Type() != HASH_OBJECT_TYPE_WAIT_OBJECT)
			return NULL;
		return static_cast<WaitObject*>(hashObject);
	}

	ThreadWaitObject* ThreadWaitObjectFor(thread_id thread, uint32 type,
		void* object) const
	{
		HashObject* hashObject = Lookup(ThreadWaitObjectKey(thread, type, object));
		if (hashObject == NULL
			|| hashObject->Type() != HASH_OBJECT_TYPE_THREAD_WAIT_OBJECT) {
			return NULL;
		}
		return static_cast<ThreadWaitObject*>(hashObject);
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

		int32 count = 0;
		for (uint32 i = 0; i < fHashTableSize; i++) {
			HashObject* object = fHashTable[i];
			while (object != NULL) {
				if (object->Type() == HASH_OBJECT_TYPE_WAIT_OBJECT) {
					WaitObject* waitObject = static_cast<WaitObject*>(object);
					if (waitObject->name[0] == '\0')
						count++;
				}

				object = object->next;
			}
		}

		return count;
	}

	status_t FinishAnalysis()
	{
		scheduling_analysis_thread** threads
			= (scheduling_analysis_thread**)Allocate(
				sizeof(Thread*) * fAnalysis.thread_count);
		if (threads == NULL)
			return B_NO_MEMORY;

		int32 index = 0;
		if (fHashTable == NULL)
			return B_OK;

		for (uint32 i = 0; i < fHashTableSize; i++) {
			HashObject* object = fHashTable[i];
			while (object != NULL) {
				switch (object->Type()) {
					case HASH_OBJECT_TYPE_THREAD:
						if (index >= (int32)fAnalysis.thread_count) {
							dprintf("scheduling_analysis: more threads found in"
								" hash table than expected (%" B_PRId32 " > %"
								B_PRId32 "); truncating\n",
								index + 1, (int32)fAnalysis.thread_count);
							break;
						}
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
				mutex* lock = (mutex*)waitObject->object;
				if (!_IsInKernelImage(lock))
					break;

				strlcpy(waitObject->name, lock->name, sizeof(waitObject->name));
				break;
			}

			case THREAD_BLOCK_TYPE_RW_LOCK:
			{
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

	uintptr_t	fNextAllocation __attribute__((aligned(8)));
	size_t		fRemainingBytes __attribute__((aligned(8)));
};


static status_t
analyze_scheduling(bigtime_t from, bigtime_t until,
	SchedulingAnalysisManager& manager)
{
	TraceEntryIterator iterator;
	iterator.MoveTo(INT_MAX);
	while (TraceEntry* _entry = iterator.Previous()) {
		if (!tracing_is_entry_valid((AbstractTraceEntry*)_entry))
			continue;

		AbstractTraceEntry* baseEntry = (AbstractTraceEntry*)_entry;
		if (baseEntry->Time() >= until)
			continue;
		if (baseEntry->Time() < from)
			break;

		uint16 entryType = baseEntry->EntryType();

		if (entryType < SCHEDULER_TRACE_ENTRY_TYPE_ENQUEUE_THREAD
			|| entryType > SCHEDULER_TRACE_ENTRY_TYPE_SCHEDULE_THREAD) {
			continue;
		}

		SchedulerTraceEntry* schedulerEntry = (SchedulerTraceEntry*)baseEntry;

		status_t error = manager.AddThread(schedulerEntry->ThreadID(),
			schedulerEntry->Name());
		if (error != B_OK)
			return error;

		if (entryType == SCHEDULER_TRACE_ENTRY_TYPE_SCHEDULE_THREAD) {
			ScheduleThread* entry = (ScheduleThread*)baseEntry;
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
		if (!tracing_is_entry_valid((AbstractTraceEntry*)_entry))
			continue;

#if SCHEDULING_ANALYSIS_TRACING
		AbstractTraceEntry* abstractEntry = (AbstractTraceEntry*)_entry;
		uint16 entryType = abstractEntry->EntryType();

		// Check if it's one of ours.
		if (entryType >= WAIT_OBJECT_TRACE_ENTRY_TYPE_CREATE_SEMAPHORE
			&& entryType <= WAIT_OBJECT_TRACE_ENTRY_TYPE_INIT_RW_LOCK) {
			WaitObjectTraceEntry* waitObjectEntry = (WaitObjectTraceEntry*)abstractEntry;
			status_t error = manager.UpdateWaitObject(waitObjectEntry->Type(),
				waitObjectEntry->Object(), waitObjectEntry->Name(),
				waitObjectEntry->ReferencedObject());
			if (error != B_OK)
				return error;
			continue;
		}
#endif

		AbstractTraceEntry* baseEntry = (AbstractTraceEntry*)_entry;
		if (baseEntry->Time() >= until)
			break;

		uint16 entryType = baseEntry->EntryType();

		if (entryType == SCHEDULER_TRACE_ENTRY_TYPE_SCHEDULE_THREAD) {
			ScheduleThread* entry = (ScheduleThread*)baseEntry;
			Thread* thread = manager.ThreadFor(entry->ThreadID());

			bigtime_t diffTime = entry->Time() - thread->lastTime;

			if (thread->state == READY) {
				thread->latencies++;
				thread->total_latency += diffTime;
				if (thread->min_latency < 0 || diffTime < thread->min_latency)
					thread->min_latency = diffTime;
				if (diffTime > thread->max_latency)
					thread->max_latency = diffTime;
			} else if (thread->state == PREEMPTED) {
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
				thread->state = RUNNING;
			}

			if (thread->state != RUNNING) {
				thread->lastTime = entry->Time();
				thread->state = RUNNING;
			}

			if (entry->ThreadID() == entry->PreviousThreadID())
				continue;

			thread = manager.ThreadFor(entry->PreviousThreadID());

			diffTime = entry->Time() - thread->lastTime;

			if (thread->state == STILL_RUNNING) {
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
		} else if (entryType == SCHEDULER_TRACE_ENTRY_TYPE_ENQUEUE_THREAD) {
			EnqueueThread* entry = (EnqueueThread*)baseEntry;
			Thread* thread = manager.ThreadFor(entry->ThreadID());

			if (thread->state == RUNNING || thread->state == STILL_RUNNING) {
				thread->state = STILL_RUNNING;
			} else {
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
		} else if (entryType == SCHEDULER_TRACE_ENTRY_TYPE_REMOVE_THREAD) {
			RemoveThread* entry = (RemoveThread*)baseEntry;
			Thread* thread = manager.ThreadFor(entry->ThreadID());

			bigtime_t diffTime = entry->Time() - thread->lastTime;
			if (thread->state == RUNNING) {
				thread->runs++;
				thread->total_run_time += diffTime;
				if (thread->min_run_time < 0 || diffTime < thread->min_run_time)
					thread->min_run_time = diffTime;
				if (diffTime > thread->max_run_time)
					thread->max_run_time = diffTime;
			} else if (thread->state == READY || thread->state == PREEMPTED) {
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
			if (!tracing_is_entry_valid((AbstractTraceEntry*)_entry))
				continue;

			AbstractTraceEntry* abstractEntry = (AbstractTraceEntry*)_entry;
			uint16 entryType = abstractEntry->EntryType();
			if (entryType >= WAIT_OBJECT_TRACE_ENTRY_TYPE_CREATE_SEMAPHORE
				&& entryType <= WAIT_OBJECT_TRACE_ENTRY_TYPE_INIT_RW_LOCK) {
				WaitObjectTraceEntry* waitObjectEntry = (WaitObjectTraceEntry*)abstractEntry;
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
