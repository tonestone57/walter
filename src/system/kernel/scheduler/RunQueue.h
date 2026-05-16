/*
 * Copyright 2013-2025 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Paweł Dziepak, pdziepak@quarnos.org
 *      Jules (2025 Array-Based EEVDF Heap implementation)
 */
#ifndef RUN_QUEUE_H
#define RUN_QUEUE_H

#include <util/BitUtils.h>
#include <util/atomic.h>

#include "scheduler_profiler.h"

template <typename Element>
struct RunQueueLink {
	RunQueueLink();

	int32 fIndex; // Positive for Eligible, Negative for Ineligible (-idx-1)
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

const int32 kMaxThreadsPerCore = 1024;

template <typename Element, unsigned int MaxPriority, typename Compare,
		  typename GetLink = RunQueueStandardGetLink<Element> >
class RunQueue {
	typedef Scheduler::RunQueueTraits<Element> Traits;

public:
	inline bool IsEmpty() const { return LoadAcquire(fTotalCount) == 0; }

	RunQueue();

	inline status_t GetInitStatus() { return fInitStatus; }

	// Ensure the heap array has enough space.
	// Fixed-size avoids unsafe realloc in kernel hotpath.
	status_t CheckCapacity(int32 count) {
		return (count <= kMaxThreadsPerCore) ? B_OK : B_DEVICE_FULL;
	}

	void CheckEligibility(bigtime_t svt);

	// Eligible threads are ordered by virtual deadline (Compare).
	inline Element* PeekRoot() const {
		return fEligibleCount > 0 ? fEligibleHeap[0] : NULL;
	}

	inline Element* PeekMaximum() const { return PeekRoot(); }
	inline Element* PeekBest() const { return PeekRoot(); }

	void PushBack(Element* element, unsigned int priority, bigtime_t svt = 0);
	void PushFront(Element* element, unsigned int priority, bigtime_t svt = 0);

	void Remove(Element* element);

	inline int32 Count() const { return LoadAcquire(fTotalCount); }

	class ConstIterator {
	public:
		ConstIterator() : fList(NULL), fIndex(0) {}
		ConstIterator(const RunQueue* list) : fList(list), fIndex(0) {}

		bool HasNext() const { return fIndex < fList->Count(); }
		Element* Next();
		void Rewind() { fIndex = 0; }

	private:
		const RunQueue* fList;
		int32 fIndex;
	};

	inline ConstIterator GetConstIterator() const { return ConstIterator(this); }

	inline Element* GetHead(unsigned int priority) const;

	template <typename Predicate>
	Element* PeekOption(const Predicate& predicate) const;

	template <typename Compare2, typename Predicate>
	Element* PeekBest(const Compare2& compare,
					  const Predicate& predicate) const;

private:
	void _BubbleUp(Element** heap, int32 count, int32 index, bool eligible);
	void _BubbleDown(Element** heap, int32 count, int32 index, bool eligible);
	void _Swap(Element** heap, int32 i, int32 j, bool eligible);

	bool _CompareIneligible(Element* a, Element* b) const {
		// Ineligible heap is ordered by VirtualRuntime (earliest first)
		return (a->GetVirtualRuntime() - b->GetVirtualRuntime()) < 0;
	}

	status_t fInitStatus;

	Element* fEligibleHeap[kMaxThreadsPerCore];
	int32 fEligibleCount;

	Element* fIneligibleHeap[kMaxThreadsPerCore];
	int32 fIneligibleCount;

	int32 fTotalCount __attribute__((aligned(8)));

	// Prevent false sharing
	char _pad0[64] __attribute__((aligned(64)));

	static GetLink sGetLink;
	static Compare sCompare;
};

template <typename Element>
RunQueueLink<Element>::RunQueueLink()
	: fIndex(0x7FFFFFFF), fPriority(0) {}

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
	: fInitStatus(B_OK), fEligibleCount(0), fIneligibleCount(0), fTotalCount(0) {
	memset(fEligibleHeap, 0, sizeof(fEligibleHeap));
	memset(fIneligibleHeap, 0, sizeof(fIneligibleHeap));
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::CheckEligibility(bigtime_t svt) {
	while (fIneligibleCount > 0) {
		Element* root = fIneligibleHeap[0];
		if (root->IsEligible(svt)) {
			// Remove from Ineligible
			int32 lastIdx = --fIneligibleCount;
			Element* last = fIneligibleHeap[lastIdx];
			if (0 != lastIdx) {
				fIneligibleHeap[0] = last;
				sGetLink(last)->fIndex = -1; // Temp index for bubble
				_BubbleDown(fIneligibleHeap, fIneligibleCount, 0, false);
			}
			sGetLink(root)->fIndex = 0x7FFFFFFF;

			// Add to Eligible
			int32 idx = fEligibleCount++;
			fEligibleHeap[idx] = root;
			sGetLink(root)->fIndex = idx;
			_BubbleUp(fEligibleHeap, fEligibleCount, idx, true);
		} else {
			break;
		}
	}
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::PushBack(Element* element, unsigned int priority, bigtime_t svt) {
	SCHEDULER_ENTER_FUNCTION();

	RunQueueLink<Element>* link = sGetLink(element);
	link->fPriority = priority;

	Traits::SetInRunQueue(element, true);
	AddAcquireRelease(fTotalCount, 1);

	if (element->IsRealTime() || element->IsIdle() || element->IsEligible(svt)) {
		int32 idx = fEligibleCount++;
		fEligibleHeap[idx] = element;
		link->fIndex = idx;
		_BubbleUp(fEligibleHeap, fEligibleCount, idx, true);
	} else {
		int32 idx = fIneligibleCount++;
		fIneligibleHeap[idx] = element;
		link->fIndex = -idx - 1;
		_BubbleUp(fIneligibleHeap, fIneligibleCount, idx, false);
	}
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::PushFront(Element* element, unsigned int priority, bigtime_t svt) {
	PushBack(element, priority, svt);
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::Remove(Element* element) {
	SCHEDULER_ENTER_FUNCTION();

	RunQueueLink<Element>* link = sGetLink(element);
	int32 rawIndex = link->fIndex;

	if (rawIndex == 0x7FFFFFFF) return;

	bool eligible = (rawIndex >= 0);
	int32 index = eligible ? rawIndex : (-rawIndex - 1);
	Element** heap = eligible ? fEligibleHeap : fIneligibleHeap;
	int32& count = eligible ? fEligibleCount : fIneligibleCount;

	if (index < 0 || index >= count || heap[index] != element)
		return;

	int32 lastIndex = --count;
	Element* last = heap[lastIndex];

	if (index != lastIndex) {
		heap[index] = last;
		sGetLink(last)->fIndex = eligible ? index : (-index - 1);

		_BubbleUp(heap, count, index, eligible);
		_BubbleDown(heap, count, index, eligible);
	}

	link->fIndex = 0x7FFFFFFF;
	Traits::SetInRunQueue(element, false);
	SubAcquireRelease(fTotalCount, 1);
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::_BubbleUp(Element** heap, int32 count, int32 index, bool eligible) {
	while (index > 0) {
		int32 parent = (index - 1) / 2;
		bool better = eligible ? sCompare(heap[index], heap[parent]) : _CompareIneligible(heap[index], heap[parent]);
		if (better) {
			_Swap(heap, index, parent, eligible);
			index = parent;
		} else
			break;
	}
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::_BubbleDown(Element** heap, int32 count, int32 index, bool eligible) {
	while (true) {
		int32 left = 2 * index + 1;
		int32 right = 2 * index + 2;
		int32 smallest = index;

		if (left < count) {
			bool better = eligible ? sCompare(heap[left], heap[smallest]) : _CompareIneligible(heap[left], heap[smallest]);
			if (better) smallest = left;
		}
		if (right < count) {
			bool better = eligible ? sCompare(heap[right], heap[smallest]) : _CompareIneligible(heap[right], heap[smallest]);
			if (better) smallest = right;
		}

		if (smallest != index) {
			_Swap(heap, index, smallest, eligible);
			index = smallest;
		} else
			break;
	}
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::_Swap(Element** heap, int32 i, int32 j, bool eligible) {
	Element* temp = heap[i];
	heap[i] = heap[j];
	heap[j] = temp;

	sGetLink(heap[i])->fIndex = eligible ? i : (-i - 1);
	sGetLink(heap[j])->fIndex = eligible ? j : (-j - 1);
}

RUN_QUEUE_TEMPLATE_LIST
Element* RUN_QUEUE_CLASS_NAME::ConstIterator::Next() {
	if (fIndex < fList->fEligibleCount)
		return fList->fEligibleHeap[fIndex++];

	int32 ineligIdx = fIndex - fList->fEligibleCount;
	if (ineligIdx < fList->fIneligibleCount) {
		fIndex++;
		return fList->fIneligibleHeap[ineligIdx];
	}
	return NULL;
}

RUN_QUEUE_TEMPLATE_LIST
Element* RUN_QUEUE_CLASS_NAME::GetHead(unsigned int priority) const {
	for (int32 i = 0; i < fEligibleCount; i++) {
		if (fEligibleHeap[i]->GetEffectivePriority() == (int32)priority)
			return fEligibleHeap[i];
	}
	for (int32 i = 0; i < fIneligibleCount; i++) {
		if (fIneligibleHeap[i]->GetEffectivePriority() == (int32)priority)
			return fIneligibleHeap[i];
	}
	return NULL;
}

RUN_QUEUE_TEMPLATE_LIST
template <typename Predicate>
Element* RUN_QUEUE_CLASS_NAME::PeekOption(const Predicate& predicate) const {
	for (int32 i = 0; i < fEligibleCount; i++) {
		if (predicate(fEligibleHeap[i]))
			return fEligibleHeap[i];
	}
	return NULL;
}

RUN_QUEUE_TEMPLATE_LIST
template <typename Compare2, typename Predicate>
Element* RUN_QUEUE_CLASS_NAME::PeekBest(const Compare2& compare,
										const Predicate& predicate) const {
	Element* best = NULL;
	for (int32 i = 0; i < fEligibleCount; i++) {
		if (predicate(fEligibleHeap[i])) {
			if (best == NULL || compare(fEligibleHeap[i], best))
				best = fEligibleHeap[i];
		}
	}
	return best;
}

RUN_QUEUE_TEMPLATE_LIST
GetLink RUN_QUEUE_CLASS_NAME::sGetLink;

RUN_QUEUE_TEMPLATE_LIST
Compare RUN_QUEUE_CLASS_NAME::sCompare;

#endif	// RUN_QUEUE_H
