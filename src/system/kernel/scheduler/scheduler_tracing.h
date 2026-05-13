/*
 * Copyright 2008-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2007, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_TRACING_H
#define KERNEL_SCHEDULER_TRACING_H


#include <arch/debug.h>
#include <cpu.h>
#include <thread.h>
#include <tracing.h>


#if SCHEDULER_TRACING

namespace SchedulerTracing {

// Manual RTTI IDs for scheduler tracing.
// Range 100-199 reserved for SchedulerTracing namespace.
enum SchedulerTraceEntryType {
	SCHEDULER_TRACE_ENTRY_TYPE_ENQUEUE_THREAD = 100,
	SCHEDULER_TRACE_ENTRY_TYPE_REMOVE_THREAD,
	SCHEDULER_TRACE_ENTRY_TYPE_SCHEDULE_THREAD,
};

class SchedulerTraceEntry : public AbstractTraceEntry {
public:
	SchedulerTraceEntry(Thread* thread)
		:
		fID(thread->id)
	{
	}

	thread_id ThreadID() const	{ return fID; }

	virtual const char* Name() const = 0;
	virtual uint16 EntryType() const = 0;

protected:
	thread_id			fID;
};


class EnqueueThread : public SchedulerTraceEntry {
public:
	virtual uint16 EntryType() const
	{
		return SCHEDULER_TRACE_ENTRY_TYPE_ENQUEUE_THREAD;
	}

	EnqueueThread(Thread* thread, int32 effectivePriority)
		:
		SchedulerTraceEntry(thread),
		fPriority(thread->priority),
		fEffectivePriority(effectivePriority)
	{
		fName = alloc_tracing_buffer_strcpy(thread->name, B_OS_NAME_LENGTH,
			false);
		Initialized();
	}

	virtual void AddDump(TraceOutput& out);

	virtual const char* Name() const;

private:
	char*				fName;
	int32				fPriority;
	int32				fEffectivePriority;
};


class RemoveThread : public SchedulerTraceEntry {
public:
	virtual uint16 EntryType() const
	{
		return SCHEDULER_TRACE_ENTRY_TYPE_REMOVE_THREAD;
	}

	RemoveThread(Thread* thread)
		:
		SchedulerTraceEntry(thread),
		fPriority(thread->priority)
	{
		Initialized();
	}

	virtual void AddDump(TraceOutput& out);

	virtual const char* Name() const;

private:
	int32				fPriority;
};


class ScheduleThread : public SchedulerTraceEntry {
public:
	virtual uint16 EntryType() const
	{
		return SCHEDULER_TRACE_ENTRY_TYPE_SCHEDULE_THREAD;
	}

	ScheduleThread(Thread* thread, Thread* previous)
		:
		SchedulerTraceEntry(thread),
		fPreviousID(previous->id),
		fCPU(previous->cpu->cpu_num),
		fPriority(thread->priority),
		fPreviousState(previous->state),
		fPreviousWaitObjectType(previous->wait.type)
	{
		fName = alloc_tracing_buffer_strcpy(thread->name, B_OS_NAME_LENGTH,
			false);

#if SCHEDULER_TRACING >= 2
		if (fPreviousState == B_THREAD_READY)
			fPreviousPC = arch_debug_get_interrupt_pc(NULL);
		else
#endif
		{
			if (fPreviousWaitObjectType == THREAD_BLOCK_TYPE_OTHER) {
				fPreviousWaitObject = alloc_tracing_buffer_strcpy(
					(const char*)previous->wait.object, B_OS_NAME_LENGTH,
					false);
			} else
				fPreviousWaitObject = previous->wait.object;
		}

		Initialized();
	}

	virtual void AddDump(TraceOutput& out);

	virtual const char* Name() const;

	thread_id PreviousThreadID() const		{ return fPreviousID; }
	uint8 PreviousState() const				{ return (uint8)fPreviousState; }
	uint16 PreviousWaitObjectType() const	{ return (uint16)fPreviousWaitObjectType; }
	const void* PreviousWaitObject() const	{ return fWait.fPreviousWaitObject; }

private:
	char*				fName;
	thread_id			fPreviousID;
	int32				fCPU;
	int32				fPriority;
	uint16				fPreviousWaitObjectType;
	uint8				fPreviousState;
	union {
		const void*		fPreviousWaitObject;
		void*			fPreviousPC;
	} fWait;
};

}	// namespace SchedulerTracing

#	define T(x) \
		if (tracing_is_enabled()) { \
			new(std::nothrow) SchedulerTracing::x; \
		}
#else
#	define T(x) ;
#endif


#if SCHEDULER_TRACING

namespace SchedulerTracing {

enum ScheduleState {
	RUNNING,
	STILL_RUNNING,
	PREEMPTED,
	READY,
	WAITING,
	UNKNOWN
};

int cmd_scheduler(int argc, char** argv);

}

#endif	// SCHEDULER_TRACING

#endif	// KERNEL_SCHEDULER_TRACING_H
