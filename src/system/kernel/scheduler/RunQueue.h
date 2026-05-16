/*
 * Copyright 2013-2025 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Paweł Dziepak, pdziepak@quarnos.org
 *      Jules (2025 Array-Based Deadline Heap implementation)
 */
#ifndef RUN_QUEUE_H
#define RUN_QUEUE_H

#include <util/BitUtils.h>
#include <util/atomic.h>

#include "scheduler_profiler.h"

template <typename Element>
struct RunQueueLink {
	RunQueueLink();

	int32 fIndex;
	unsigned int fPriority;
} __attribute__((aligned(8)));

template <typename Element>
class RunQueueLinkImpl {
public:
	inline RunQueueLink<Element>* GetRunQueueLink();

private:
	RunQueueLink<Element> fRunQueueLink;
};

template <typename Element>
class RunQueueStandardGetLink {
private:
	typedef RunQueueLink<Element> Link;

public:
	inline Link* operator()(Element* element) const;
};

template <typename Element, RunQueueLink<Element> Element::*LinkMember>
class RunQueueMemberGetLink {
private:
	typedef RunQueueLink<Element> Link;

public:
	inline Link* operator()(Element* element) const;
};

namespace Scheduler {
template <typename Element>
struct RunQueueTraits {
	static inline void SetInRunQueue(Element* element, bool inQueue) {}
};
}  // namespace Scheduler

#define RUN_QUEUE_TEMPLATE_LIST                                             \
	template <typename Element, unsigned int MaxPriority, typename Compare, \
			  typename GetLink>
#define RUN_QUEUE_CLASS_NAME RunQueue<Element, MaxPriority, Compare, GetLink>

template <typename Element, unsigned int MaxPriority, typename Compare,
		  typename GetLink = RunQueueStandardGetLink<Element> >
class RunQueue {
	typedef Scheduler::RunQueueTraits<Element> Traits;

public:
	inline bool IsEmpty() const { return LoadAcquire(fTotalCount) == 0; }

	RunQueue();
	~RunQueue();

	inline status_t GetInitStatus() { return fInitStatus; }

	// Ensure the heap array has enough space for 'count' elements.
	// Must be called WITHOUT holding the run-queue spinlock.
	status_t CheckCapacity(int32 count);

	// Min-Heap Root is always at index 0.
	inline Element* PeekRoot() const {
		return fTotalCount > 0 ? fElements[0] : NULL;
	}

	// Compatibility aliases for the scheduler
	inline Element* PeekMaximum() const { return PeekRoot(); }
	inline Element* PeekBest() const { return PeekRoot(); }

	void PushBack(Element* element, unsigned int priority);
	void PushFront(Element* element, unsigned int priority);

	void Remove(Element* element);

	inline int32 Count() const { return LoadAcquire(fTotalCount); }

	class ConstIterator {
	public:
		ConstIterator() : fList(NULL), fIndex(0) {}
		ConstIterator(const RunQueue* list) : fList(list), fIndex(0) {}

		bool HasNext() const { return fIndex < fList->Count(); }
		Element* Next() {
			if (fIndex < fList->Count())
				return fList->fElements[fIndex++];
			return NULL;
		}
		void Rewind() { fIndex = 0; }

	private:
		const RunQueue* fList;
		int32 fIndex;
	};

	inline ConstIterator GetConstIterator() const { return ConstIterator(this); }

	inline Element* GetHead(unsigned int priority) const {
		for (int32 i = 0; i < fTotalCount; i++) {
			if (fElements[i]->GetEffectivePriority() == (int32)priority)
				return fElements[i];
		}
		return NULL;
	}

	template <typename Predicate>
	Element* PeekOption(const Predicate& predicate) const;

	template <typename Compare2, typename Predicate>
	Element* PeekBest(const Compare2& compare,
					  const Predicate& predicate) const;

private:
	void _BubbleUp(int32 index);
	void _BubbleDown(int32 index);
	void _Swap(int32 i, int32 j);

	status_t fInitStatus;

	Element** fElements;
	int32 fCapacity;
	int32 fTotalCount __attribute__((aligned(8)));

	// Prevent false sharing
	char _pad0[64] __attribute__((aligned(64)));

	static GetLink sGetLink;
	static Compare sCompare;
};

template <typename Element>
RunQueueLink<Element>::RunQueueLink()
	: fIndex(-1), fPriority(0) {}

template <typename Element>
RunQueueLink<Element>* RunQueueLinkImpl<Element>::GetRunQueueLink() {
	return &fRunQueueLink;
}

template <typename Element>
RunQueueLink<Element>* RunQueueStandardGetLink<Element>::operator()(
	Element* element) const {
	return element->GetRunQueueLink();
}

template <typename Element, RunQueueLink<Element> Element::*LinkMember>
RunQueueLink<Element>* RunQueueMemberGetLink<Element, LinkMember>::operator()(
	Element* element) const {
	return &(element->*LinkMember);
}

RUN_QUEUE_TEMPLATE_LIST
RUN_QUEUE_CLASS_NAME::RunQueue()
	: fInitStatus(B_OK), fElements(NULL), fCapacity(0), fTotalCount(0) {
	CheckCapacity(16); // Initial small capacity
}

RUN_QUEUE_TEMPLATE_LIST
RUN_QUEUE_CLASS_NAME::~RunQueue() {
	free(fElements);
}

RUN_QUEUE_TEMPLATE_LIST
status_t RUN_QUEUE_CLASS_NAME::CheckCapacity(int32 count) {
	if (count <= fCapacity)
		return B_OK;

	int32 newCapacity = max_c(fCapacity * 2, count);
	Element** newElements = (Element**)realloc(fElements, sizeof(Element*) * newCapacity);
	if (newElements == NULL)
		return B_NO_MEMORY;

	fElements = newElements;
	fCapacity = newCapacity;
	return B_OK;
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::PushBack(Element* element, unsigned int priority) {
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(fTotalCount < fCapacity);

	RunQueueLink<Element>* link = sGetLink(element);
	link->fPriority = priority;

	int32 index = fTotalCount++;
	fElements[index] = element;
	link->fIndex = index;

	Traits::SetInRunQueue(element, true);

	_BubbleUp(index);

	// Note: total count update is atomic for lockless IsEmpty() check.
	// Since we hold the spinlock, the array update and fTotalCount increment
	// are consistent for the writer.
	StoreRelease(fTotalCount, fTotalCount);
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::PushFront(Element* element, unsigned int priority) {
	PushBack(element, priority);
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::Remove(Element* element) {
	SCHEDULER_ENTER_FUNCTION();

	RunQueueLink<Element>* link = sGetLink(element);
	int32 index = link->fIndex;

	if (index < 0 || index >= fTotalCount || fElements[index] != element)
		return;

	int32 lastIndex = --fTotalCount;
	Element* last = fElements[lastIndex];

	if (index != lastIndex) {
		fElements[index] = last;
		sGetLink(last)->fIndex = index;

		_BubbleUp(index);
		_BubbleDown(index);
	}

	link->fIndex = -1;
	Traits::SetInRunQueue(element, false);

	StoreRelease(fTotalCount, fTotalCount);
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::_BubbleUp(int32 index) {
	while (index > 0) {
		int32 parent = (index - 1) / 2;
		if (sCompare(fElements[index], fElements[parent])) {
			_Swap(index, parent);
			index = parent;
		} else
			break;
	}
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::_BubbleDown(int32 index) {
	while (true) {
		int32 left = 2 * index + 1;
		int32 right = 2 * index + 2;
		int32 smallest = index;

		if (left < fTotalCount && sCompare(fElements[left], fElements[smallest]))
			smallest = left;
		if (right < fTotalCount && sCompare(fElements[right], fElements[smallest]))
			smallest = right;

		if (smallest != index) {
			_Swap(index, smallest);
			index = smallest;
		} else
			break;
	}
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::_Swap(int32 i, int32 j) {
	Element* temp = fElements[i];
	fElements[i] = fElements[j];
	fElements[j] = temp;

	sGetLink(fElements[i])->fIndex = i;
	sGetLink(fElements[j])->fIndex = j;
}

RUN_QUEUE_TEMPLATE_LIST
template <typename Predicate>
Element* RUN_QUEUE_CLASS_NAME::PeekOption(const Predicate& predicate) const {
	// Linear scan for predicate matches. Heaps are not good for general searching,
	// but the search limit was small anyway.
	int32 searchLimit = min_c(fTotalCount, (int32)64);
	for (int32 i = 0; i < searchLimit; i++) {
		if (predicate(fElements[i]))
			return fElements[i];
	}
	return NULL;
}

RUN_QUEUE_TEMPLATE_LIST
template <typename Compare2, typename Predicate>
Element* RUN_QUEUE_CLASS_NAME::PeekBest(const Compare2& compare,
										const Predicate& predicate) const {
	Element* best = NULL;
	int32 searchLimit = min_c(fTotalCount, (int32)64);
	for (int32 i = 0; i < searchLimit; i++) {
		if (predicate(fElements[i])) {
			if (best == NULL || compare(fElements[i], best))
				best = fElements[i];
		}
	}
	return best;
}

RUN_QUEUE_TEMPLATE_LIST
GetLink RUN_QUEUE_CLASS_NAME::sGetLink;

RUN_QUEUE_TEMPLATE_LIST
Compare RUN_QUEUE_CLASS_NAME::sCompare;

#endif	// RUN_QUEUE_H
