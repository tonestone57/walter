/*
 * Copyright 2013-2025 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Paweł Dziepak, pdziepak@quarnos.org
 *      Jules (2025 Deadline Heap implementation)
 */
#ifndef RUN_QUEUE_H
#define RUN_QUEUE_H

#include <util/BitUtils.h>
#include <util/atomic.h>

#include "scheduler_profiler.h"

template <typename Element>
struct RunQueueLink {
	RunQueueLink();

	Element* fParent;
	Element* fLeft;
	Element* fRight;

	// Cached for comparison in some cases, though Compare usually reads from Element.
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

	inline status_t GetInitStatus() { return fInitStatus; }

	// Min-Heap Root is always the best element.
	inline Element* PeekRoot() const { return atomic_pointer_get<Element>(&fRoot); }

	// Compatibility aliases for the scheduler
	inline Element* PeekMaximum() const { return PeekRoot(); }
	inline Element* PeekBest() const { return PeekRoot(); }

	void PushBack(Element* element, unsigned int priority);
	void PushFront(Element* element, unsigned int priority);

	void Remove(Element* element);

	inline int32 Count() const { return LoadAcquire(fTotalCount); }

	// Note: Iteration in a heap is not priority-ordered unless we extract everything.
	// For now, providing a basic iterator that does a tree traversal.
	class ConstIterator {
	public:
		ConstIterator() : fList(NULL), fCurrent(NULL), fIndex(0) {}
		ConstIterator(const RunQueue* list) : fList(list), fCurrent(NULL), fIndex(0) { Rewind(); }

		bool HasNext() const { return fIndex < fList->Count(); }
		Element* Next();
		void Rewind();

	private:
		const RunQueue* fList;
		Element* fCurrent;
		int32 fIndex;
	};

	inline ConstIterator GetConstIterator() const { return ConstIterator(this); }

	inline Element* GetHead(unsigned int priority) const {
		struct PriorityPredicate {
			unsigned int priority;
			PriorityPredicate(unsigned int p) : priority(p) {}
			bool operator()(Element* e) const {
				return e->GetEffectivePriority() == (int32)priority;
			}
		} predicate(priority);
		return PeekBest(Compare(), predicate);
	}

	template <typename Predicate>
	Element* PeekOption(const Predicate& predicate) const;

	template <typename Compare2, typename Predicate>
	Element* PeekBest(const Compare2& compare,
					  const Predicate& predicate) const;

private:
	Element* _GetNodeAt(int32 index) const;
	void _BubbleUp(Element* element);
	void _BubbleDown(Element* element);
	void _Swap(Element* a, Element* b);

	status_t fInitStatus;

	mutable Element* fRoot __attribute__((aligned(8)));
	int32 fTotalCount __attribute__((aligned(8)));

	// Prevent false sharing
	char _pad0[64] __attribute__((aligned(64)));

	static GetLink sGetLink;
	static Compare sCompare;
};

template <typename Element>
RunQueueLink<Element>::RunQueueLink()
	: fParent(NULL), fLeft(NULL), fRight(NULL), fPriority(0) {}

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
	: fInitStatus(B_OK), fRoot(NULL), fTotalCount(0) {}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::PushBack(Element* element, unsigned int priority) {
	SCHEDULER_ENTER_FUNCTION();

	RunQueueLink<Element>* link = sGetLink(element);
	link->fParent = link->fLeft = link->fRight = NULL;
	link->fPriority = priority;

	int32 count = AddAcquireRelease(fTotalCount, 1);
	int32 index = count + 1;

	Traits::SetInRunQueue(element, true);

	if (index == 1) {
		atomic_pointer_set<Element>(&fRoot, element);
		return;
	}

	// Navigate to the parent of the new node.
	// index is the 1-based index of the new node.
	// Its parent is at index / 2.
	Element* parent = _GetNodeAt(index / 2);
	RunQueueLink<Element>* parentLink = sGetLink(parent);
	link->fParent = parent;

	if (index & 1)
		atomic_pointer_set<Element>(&parentLink->fRight, element);
	else
		atomic_pointer_set<Element>(&parentLink->fLeft, element);

	_BubbleUp(element);
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::PushFront(Element* element, unsigned int priority) {
	// In a min-heap, PushFront and PushBack both just insert and maintain heap property.
	PushBack(element, priority);
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::Remove(Element* element) {
	SCHEDULER_ENTER_FUNCTION();

	int32 count = LoadAcquire(fTotalCount);
	if (count <= 0) return;

	Element* root = atomic_pointer_get<Element>(&fRoot);
	if (count == 1) {
		ASSERT(root == element);
		atomic_pointer_set<Element>(&fRoot, (Element*)NULL);
		SubAcquireRelease(fTotalCount, 1);
		Traits::SetInRunQueue(element, false);
		sGetLink(element)->fParent = sGetLink(element)->fLeft = sGetLink(element)->fRight = NULL;
		return;
	}

	// Find the last element in the heap.
	Element* last = _GetNodeAt(count);
	ASSERT(last != NULL);

	if (last == element) {
		// Just unlink the last element.
		RunQueueLink<Element>* lastLink = sGetLink(last);
		Element* parent = lastLink->fParent;
		if (parent != NULL) {
			if (sGetLink(parent)->fLeft == last)
				atomic_pointer_set<Element>(&sGetLink(parent)->fLeft, (Element*)NULL);
			else
				atomic_pointer_set<Element>(&sGetLink(parent)->fRight, (Element*)NULL);
		}
		SubAcquireRelease(fTotalCount, 1);
		Traits::SetInRunQueue(element, false);
		lastLink->fParent = lastLink->fLeft = lastLink->fRight = NULL;
		return;
	}

	// Swap 'element' with 'last', then remove 'last'.
	_Swap(element, last);
	// Now 'element' is at the position 'last' used to be.
	// 'last' is at the position 'element' used to be.

	// Remove 'element' from its new position (which is the last position).
	RunQueueLink<Element>* elementLink = sGetLink(element);
	Element* parent = elementLink->fParent;
	if (parent != NULL) {
		if (sGetLink(parent)->fLeft == element)
			atomic_pointer_set<Element>(&sGetLink(parent)->fLeft, (Element*)NULL);
		else
			atomic_pointer_set<Element>(&sGetLink(parent)->fRight, (Element*)NULL);
	}

	SubAcquireRelease(fTotalCount, 1);
	Traits::SetInRunQueue(element, false);
	elementLink->fParent = elementLink->fLeft = elementLink->fRight = NULL;

	// Maintain heap property for 'last' at its new position.
	_BubbleUp(last);
	_BubbleDown(last);
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::_BubbleUp(Element* element) {
	RunQueueLink<Element>* link = sGetLink(element);
	while (link->fParent != NULL && sCompare(element, link->fParent)) {
		_Swap(link->fParent, element);
		// link->fParent changed after swap, but we still want to bubble 'element' up.
		// _Swap swaps the positions in the tree.
	}
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::_BubbleDown(Element* element) {
	RunQueueLink<Element>* link = sGetLink(element);
	while (true) {
		Element* left = atomic_pointer_get<Element>(&link->fLeft);
		Element* right = atomic_pointer_get<Element>(&link->fRight);
		Element* best = element;

		if (left != NULL && sCompare(left, best))
			best = left;
		if (right != NULL && sCompare(right, best))
			best = right;

		if (best == element)
			break;

		_Swap(element, best);
	}
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::_Swap(Element* a, Element* b) {
	if (a == b) return;

	RunQueueLink<Element>* la = sGetLink(a);
	RunQueueLink<Element>* lb = sGetLink(b);

	Element* pA = la->fParent;
	Element* lA = la->fLeft;
	Element* rA = la->fRight;

	Element* pB = lb->fParent;
	Element* lB = lb->fLeft;
	Element* rB = lb->fRight;

	// Handle adjacency
	if (pB == a) {
		// b is child of a
		lb->fParent = pA;
		la->fParent = b;
		la->fLeft = lB;
		la->fRight = rB;
		if (lB) sGetLink(lB)->fParent = a;
		if (rB) sGetLink(rB)->fParent = a;

		if (lA == b) {
			lb->fLeft = a;
			lb->fRight = rA;
			if (rA) sGetLink(rA)->fParent = b;
		} else {
			lb->fLeft = lA;
			lb->fRight = a;
			if (lA) sGetLink(lA)->fParent = b;
		}
	} else if (pA == b) {
		// a is child of b
		_Swap(b, a);
		return;
	} else {
		// General case
		la->fParent = pB;
		la->fLeft = lB;
		la->fRight = rB;
		if (lB) sGetLink(lB)->fParent = a;
		if (rB) sGetLink(rB)->fParent = a;

		lb->fParent = pA;
		lb->fLeft = lA;
		lb->fRight = rA;
		if (lA) sGetLink(lA)->fParent = b;
		if (rA) sGetLink(rA)->fParent = b;
	}

	// Update parents of a and b
	if (la->fParent) {
		if (sGetLink(la->fParent)->fLeft == b)
			atomic_pointer_set<Element>(&sGetLink(la->fParent)->fLeft, a);
		else
			atomic_pointer_set<Element>(&sGetLink(la->fParent)->fRight, a);
	} else {
		atomic_pointer_set<Element>(&fRoot, a);
	}

	if (lb->fParent) {
		if (sGetLink(lb->fParent)->fLeft == a)
			atomic_pointer_set<Element>(&sGetLink(lb->fParent)->fLeft, b);
		else
			atomic_pointer_set<Element>(&sGetLink(lb->fParent)->fRight, b);
	} else {
		atomic_pointer_set<Element>(&fRoot, b);
	}
}

RUN_QUEUE_TEMPLATE_LIST
Element* RUN_QUEUE_CLASS_NAME::_GetNodeAt(int32 index) const {
	if (index <= 0 || index > LoadAcquire(fTotalCount))
		return NULL;

	if (index == 1)
		return atomic_pointer_get<Element>(&fRoot);

	// The path to the node at index 'index' can be determined by its binary representation.
	// For example, index 6 (binary 110). Root is 1. Skip the MSB.
	// 1 (next bit) -> right child. 0 (next bit) -> left child.
	int msb = fls(index) - 1;
	Element* current = atomic_pointer_get<Element>(&fRoot);
	for (int i = msb - 1; i >= 0; i--) {
		if (index & (1 << i))
			current = atomic_pointer_get<Element>(&sGetLink(current)->fRight);
		else
			current = atomic_pointer_get<Element>(&sGetLink(current)->fLeft);
	}
	return current;
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::ConstIterator::Rewind() {
	fCurrent = atomic_pointer_get<Element>(&fList->fRoot);
	fIndex = 0;
}

RUN_QUEUE_TEMPLATE_LIST
Element* RUN_QUEUE_CLASS_NAME::ConstIterator::Next() {
	// Level-order traversal using _GetNodeAt for simplicity,
	// although it's O(N log N) for full iteration.
	// Given typical run queue sizes in Haiku, this is acceptable for debug/tracing.
	Element* res = fList->_GetNodeAt(++fIndex);
	return res;
}

RUN_QUEUE_TEMPLATE_LIST
template <typename Predicate>
Element* RUN_QUEUE_CLASS_NAME::PeekOption(const Predicate& predicate) const {
	// DFS with limited depth/budget to find an element matching predicate.
	// Since it's a min-heap, we'll find better elements earlier.
	if (IsEmpty()) return NULL;

	Element* stack[64];
	int32 stackPtr = 0;
	stack[stackPtr++] = atomic_pointer_get<Element>(&fRoot);

	int32 budget = 64; // Limit search to 64 nodes.

	while (stackPtr > 0 && budget-- > 0) {
		Element* current = stack[--stackPtr];
		if (predicate(current))
			return current;

		RunQueueLink<Element>* link = sGetLink(current);
		Element* right = atomic_pointer_get<Element>(&link->fRight);
		if (right && stackPtr < 64)
			stack[stackPtr++] = right;
		Element* left = atomic_pointer_get<Element>(&link->fLeft);
		if (left && stackPtr < 64)
			stack[stackPtr++] = left;
	}

	return NULL;
}

RUN_QUEUE_TEMPLATE_LIST
template <typename Compare2, typename Predicate>
Element* RUN_QUEUE_CLASS_NAME::PeekBest(const Compare2& compare,
										const Predicate& predicate) const {
	// Find the best element matching predicate.
	if (IsEmpty()) return NULL;

	Element* stack[64];
	int32 stackPtr = 0;
	stack[stackPtr++] = atomic_pointer_get<Element>(&fRoot);

	Element* best = NULL;
	int32 budget = 64;

	while (stackPtr > 0 && budget-- > 0) {
		Element* current = stack[--stackPtr];
		if (predicate(current)) {
			if (best == NULL || compare(current, best))
				best = current;
		}

		// Pruning: if current node is already worse than best (using heap order),
		// its children are even worse.
		// Note: This only works if 'compare' is compatible with the heap's 'sCompare'.
		// In Haiku's scheduler, sCompare is DeadlineCompare and Compare2 is VRuntimeCompare.
		// They are mostly compatible for interactive tasks.

		RunQueueLink<Element>* link = sGetLink(current);
		Element* right = atomic_pointer_get<Element>(&link->fRight);
		if (right && stackPtr < 64)
			stack[stackPtr++] = right;
		Element* left = atomic_pointer_get<Element>(&link->fLeft);
		if (left && stackPtr < 64)
			stack[stackPtr++] = left;
	}

	return best;
}

RUN_QUEUE_TEMPLATE_LIST
GetLink RUN_QUEUE_CLASS_NAME::sGetLink;

RUN_QUEUE_TEMPLATE_LIST
Compare RUN_QUEUE_CLASS_NAME::sCompare;

#endif	// RUN_QUEUE_H
