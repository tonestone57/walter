/*
 * Copyright 2009-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_LISTENERS_H
#define KERNEL_LISTENERS_H


#include <KernelExport.h>

#include <util/atomic.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>


struct ConditionVariable;
struct mutex;
struct rw_lock;


// scheduler listeners


struct SchedulerListener {
	virtual						~SchedulerListener();

	virtual	void				ThreadEnqueuedInRunQueue(
									Thread* thread) = 0;
	virtual	void				ThreadRemovedFromRunQueue(
									Thread* thread) = 0;
	virtual	void				ThreadScheduled(Thread* oldThread,
									Thread* newThread) = 0;

			SchedulerListener* volatile fNext;
};


extern SchedulerListener* volatile gSchedulerListeners;
extern rw_spinlock gSchedulerListenersLock;


template<typename Parameter1>
inline void
NotifySchedulerListeners(void (SchedulerListener::*hook)(Parameter1),
	Parameter1 parameter1)
{
	SchedulerListener* listener = atomic_pointer_get(&gSchedulerListeners);
	while (listener != NULL) {
		(listener->*hook)(parameter1);
		listener = atomic_pointer_get(&listener->fNext);
	}
}


template<typename Parameter1, typename Parameter2>
inline void
NotifySchedulerListeners(
	void (SchedulerListener::*hook)(Parameter1, Parameter2),
	Parameter1 parameter1, Parameter2 parameter2)
{
	SchedulerListener* listener = atomic_pointer_get(&gSchedulerListeners);
	while (listener != NULL) {
		(listener->*hook)(parameter1, parameter2);
		listener = atomic_pointer_get(&listener->fNext);
	}
}


// wait object listeners


struct WaitObjectListener : DoublyLinkedListLinkImpl<WaitObjectListener> {
	virtual						~WaitObjectListener();

	virtual	void				SemaphoreCreated(sem_id id,
									const char* name) = 0;
	virtual	void				ConditionVariableInitialized(
									ConditionVariable* variable) = 0;
	virtual	void				MutexInitialized(mutex* lock) = 0;
	virtual	void				RWLockInitialized(rw_lock* lock) = 0;
};

typedef DoublyLinkedList<WaitObjectListener> WaitObjectListenerList;
extern WaitObjectListenerList gWaitObjectListeners;
extern rw_spinlock gWaitObjectListenerLock;


template<typename Parameter1>
inline void
NotifyWaitObjectListeners(void (WaitObjectListener::*hook)(Parameter1),
	Parameter1 parameter1)
{
	if (!gWaitObjectListeners.IsEmpty()) {
		InterruptsReadSpinLocker locker(gWaitObjectListenerLock);
		WaitObjectListenerList::Iterator it
			= gWaitObjectListeners.GetIterator();
		while (WaitObjectListener* listener = it.Next())
			(listener->*hook)(parameter1);
	}
}


template<typename Parameter1, typename Parameter2>
inline void
NotifyWaitObjectListeners(
	void (WaitObjectListener::*hook)(Parameter1, Parameter2),
	Parameter1 parameter1, Parameter2 parameter2)
{
	if (!gWaitObjectListeners.IsEmpty()) {
		InterruptsReadSpinLocker locker(gWaitObjectListenerLock);
		WaitObjectListenerList::Iterator it
			= gWaitObjectListeners.GetIterator();
		while (WaitObjectListener* listener = it.Next())
			(listener->*hook)(parameter1, parameter2);
	}
}


void add_wait_object_listener(struct WaitObjectListener* listener);
void remove_wait_object_listener(struct WaitObjectListener* listener);


#endif	// KERNEL_LISTENERS_H
