/*
 * Copyright 2013 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Paweł Dziepak, pdziepak@quarnos.org
 */
#ifndef RUN_QUEUE_H
#define RUN_QUEUE_H


#include <util/BitUtils.h>

#include "scheduler_profiler.h"


template<typename Element>
struct RunQueueLink {
					RunQueueLink();

	unsigned int	fPriority;
	Element*		fPrevious;
	Element*		fNext;
};

template<typename Element>
class RunQueueLinkImpl {
public:
	inline	RunQueueLink<Element>*	GetRunQueueLink();

private:
			RunQueueLink<Element>	fRunQueueLink;
};

template<typename Element>
class RunQueueStandardGetLink {
private:
	typedef RunQueueLink<Element> Link;

public:
	inline	Link*		operator()(Element* element) const;
};

template<typename Element, RunQueueLink<Element> Element::*LinkMember>
class RunQueueMemberGetLink {
private:
	typedef RunQueueLink<Element> Link;

public:
	inline	Link*		operator()(Element* element) const;
};

#define RUN_QUEUE_TEMPLATE_LIST	\
	template<typename Element, unsigned int MaxPriority, typename Compare, typename GetLink>
#define RUN_QUEUE_CLASS_NAME	RunQueue<Element, MaxPriority, Compare, GetLink>

template<typename Element, unsigned int MaxPriority, typename Compare,
	typename GetLink = RunQueueStandardGetLink<Element> >
class RunQueue {
public:
	static const int kBitmapSize = (MaxPriority / 32) + 1;

	class ConstIterator {
	public:
								ConstIterator();
								ConstIterator(const RunQueue<Element,
										MaxPriority, Compare, GetLink>* list);

		inline	ConstIterator&	operator=(const ConstIterator& other);

				bool			HasNext() const;
				Element*		Next();

				void			Rewind();

	private:
		inline	void			_FindNextPriority();

				const RUN_QUEUE_CLASS_NAME*	fList;
				unsigned int	fPriority;
				Element*		fNext;

		static	GetLink			sGetLink;
	};

						RunQueue();

	inline	status_t	GetInitStatus();

	inline	Element*	PeekMaximum() const;

	inline	void		PushFront(Element* element, unsigned int priority);
	inline	void		PushBack(Element* elementt, unsigned int priority);

	inline	void		Remove(Element* element);

	inline	Element*	GetHead(unsigned int priority) const;

	inline	const uint32*	GetBitmap() const;

	inline	ConstIterator	GetConstIterator() const;

	/*!
		Finds the best element in the highest priority non-empty queue.
	*/
	Element*	PeekBest() const;

	template<typename Predicate>
	Element*	PeekOption(const Predicate& predicate) const;

private:
			status_t	fInitStatus;

			uint32		fBitmap[kBitmapSize];

			Element*	fHeads[MaxPriority + 1];
			Element*	fTails[MaxPriority + 1];

	mutable	Element*	fBest;

	static	GetLink		sGetLink;
	static	Compare		sCompare;
};


template<typename Element>
RunQueueLink<Element>::RunQueueLink()
	:
	fPrevious(NULL),
	fNext(NULL)
{
}


template<typename Element>
RunQueueLink<Element>*
RunQueueLinkImpl<Element>::GetRunQueueLink()
{
	return &fRunQueueLink;
}


template<typename Element>
RunQueueLink<Element>*
RunQueueStandardGetLink<Element>::operator()(Element* element) const
{
	return element->GetRunQueueLink();
}


template<typename Element, RunQueueLink<Element> Element::*LinkMember>
RunQueueLink<Element>*
RunQueueMemberGetLink<Element, LinkMember>::operator()(Element* element) const
{
	return &(element->*LinkMember);
}


RUN_QUEUE_TEMPLATE_LIST
RUN_QUEUE_CLASS_NAME::ConstIterator::ConstIterator()
	:
	fList(NULL),
	fPriority(0),
	fNext(NULL)
{
}


RUN_QUEUE_TEMPLATE_LIST
RUN_QUEUE_CLASS_NAME::ConstIterator::ConstIterator(const RunQueue<Element,
		MaxPriority, Compare, GetLink>* list)
	:
	fList(list)
{
	Rewind();
}


RUN_QUEUE_TEMPLATE_LIST
typename RUN_QUEUE_CLASS_NAME::ConstIterator&
RUN_QUEUE_CLASS_NAME::ConstIterator::operator=(const ConstIterator& other)
{
	fList = other.fList;
	fPriority = other.fPriority;
	fNext = other.fNext;

	return *this;
}


RUN_QUEUE_TEMPLATE_LIST
bool
RUN_QUEUE_CLASS_NAME::ConstIterator::HasNext() const
{
	return fNext != NULL;
}


RUN_QUEUE_TEMPLATE_LIST
Element*
RUN_QUEUE_CLASS_NAME::ConstIterator::Next()
{
	ASSERT(HasNext());

	Element* current = fNext;
	RunQueueLink<Element>* link = sGetLink(fNext);

	fNext = link->fNext;
	if (fNext == NULL)
		_FindNextPriority();

	return current;
}


RUN_QUEUE_TEMPLATE_LIST
void
RUN_QUEUE_CLASS_NAME::ConstIterator::Rewind()
{
	ASSERT(fList != NULL);

	fPriority = MaxPriority;
	fNext = fList->GetHead(fPriority);
	if (fNext == NULL)
		_FindNextPriority();
}


RUN_QUEUE_TEMPLATE_LIST
void
RUN_QUEUE_CLASS_NAME::ConstIterator::_FindNextPriority()
{
	ASSERT(fList != NULL);

	const uint32* bitmap = fList->GetBitmap();

	int i = fPriority / 32;
	uint32 val = bitmap[i];

	// Mask out higher priorities (bits >= current bit index) in current word
	// because we are looking for the *next* priority lower than fPriority.
	// fPriority is the one we just finished.
	// We want to check fPriority - 1 down to 0.

	int currentBit = fPriority % 32;
	if (currentBit > 0) {
		// Mask bits at currentBit and above, keep bits 0..currentBit-1
		val &= (1UL << currentBit) - 1;
	} else {
		// If we finished bit 0, this word is done.
		val = 0;
	}

	while (true) {
		if (val != 0) {
			int bit = fls(val) - 1;
			fPriority = i * 32 + bit;
			fNext = fList->GetHead(fPriority);
			return;
		}

		if (i == 0) break;
		i--;
		val = bitmap[i];
	}

	fNext = NULL;
}


RUN_QUEUE_TEMPLATE_LIST
RUN_QUEUE_CLASS_NAME::RunQueue()
	:
	fInitStatus(B_OK),
	fBest(NULL)
{
	memset(fBitmap, 0, sizeof(fBitmap));
	memset(fHeads, 0, sizeof(fHeads));
	memset(fTails, 0, sizeof(fTails));
}


RUN_QUEUE_TEMPLATE_LIST
status_t
RUN_QUEUE_CLASS_NAME::GetInitStatus()
{
	return fInitStatus;
}


RUN_QUEUE_TEMPLATE_LIST
Element*
RUN_QUEUE_CLASS_NAME::PeekMaximum() const
{
	SCHEDULER_ENTER_FUNCTION();

	for (int i = kBitmapSize - 1; i >= 0; i--) {
		uint32 val = fBitmap[i];
		if (val != 0) {
			if (i == kBitmapSize - 1 && (MaxPriority % 32 != 31)) {
				val &= (1UL << (MaxPriority % 32 + 1)) - 1;
				if (val == 0)
					continue;
			}

			int bit = fls(val) - 1;
			unsigned int priority = i * 32 + bit;

			ASSERT(priority <= MaxPriority);
			ASSERT(fHeads[priority] != NULL);
			return fHeads[priority];
		}
	}

	return NULL;
}


RUN_QUEUE_TEMPLATE_LIST
void
RUN_QUEUE_CLASS_NAME::PushFront(Element* element,
	unsigned int priority)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(priority <= MaxPriority);

	RunQueueLink<Element>* elementLink = sGetLink(element);

	ASSERT(elementLink->fPrevious == NULL);
	ASSERT(elementLink->fNext == NULL);

	ASSERT((fHeads[priority] == NULL && fTails[priority] == NULL)
		|| (fHeads[priority] != NULL && fTails[priority] != NULL));

	elementLink->fPriority = priority;
	elementLink->fNext = fHeads[priority];
	if (fHeads[priority] != NULL)
		sGetLink(fHeads[priority])->fPrevious = element;
	else {
		fTails[priority] = element;
		fBitmap[priority / 32] |= (1UL << (priority % 32));
	}
	fHeads[priority] = element;

	if (fBest != NULL) {
		unsigned int bestPriority = sGetLink(fBest)->fPriority;
		if (priority > bestPriority)
			fBest = element;
		else if (priority == bestPriority && sCompare(element, fBest))
			fBest = element;
	}
}


RUN_QUEUE_TEMPLATE_LIST
void
RUN_QUEUE_CLASS_NAME::PushBack(Element* element,
	unsigned int priority)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(priority <= MaxPriority);

	RunQueueLink<Element>* elementLink = sGetLink(element);

	ASSERT(elementLink->fPrevious == NULL);
	ASSERT(elementLink->fNext == NULL);

	ASSERT((fHeads[priority] == NULL && fTails[priority] == NULL)
		|| (fHeads[priority] != NULL && fTails[priority] != NULL));

	elementLink->fPriority = priority;
	elementLink->fPrevious = fTails[priority];
	if (fTails[priority] != NULL)
		sGetLink(fTails[priority])->fNext = element;
	else {
		fHeads[priority] = element;
		fBitmap[priority / 32] |= (1UL << (priority % 32));
	}
	fTails[priority] = element;

	if (fBest != NULL) {
		unsigned int bestPriority = sGetLink(fBest)->fPriority;
		if (priority > bestPriority)
			fBest = element;
		else if (priority == bestPriority && sCompare(element, fBest))
			fBest = element;
	}
}


RUN_QUEUE_TEMPLATE_LIST
void
RUN_QUEUE_CLASS_NAME::Remove(Element* element)
{
	SCHEDULER_ENTER_FUNCTION();

	RunQueueLink<Element>* elementLink = sGetLink(element);
	unsigned int priority = elementLink->fPriority;

	ASSERT(elementLink->fPrevious != NULL || fHeads[priority] == element);
	ASSERT(elementLink->fNext != NULL || fTails[priority] == element);

	if (elementLink->fPrevious != NULL)
		sGetLink(elementLink->fPrevious)->fNext = elementLink->fNext;
	else
		fHeads[priority] = elementLink->fNext;
	if (elementLink->fNext != NULL)
		sGetLink(elementLink->fNext)->fPrevious = elementLink->fPrevious;
	else
		fTails[priority] = elementLink->fPrevious;

	ASSERT((fHeads[priority] == NULL && fTails[priority] == NULL)
		|| (fHeads[priority] != NULL && fTails[priority] != NULL));

	if (fHeads[priority] == NULL) {
		fBitmap[priority / 32] &= ~(1UL << (priority % 32));
	}

	elementLink->fPrevious = NULL;
	elementLink->fNext = NULL;

	if (fBest == element)
		fBest = NULL;
}


RUN_QUEUE_TEMPLATE_LIST
Element*
RUN_QUEUE_CLASS_NAME::GetHead(unsigned int priority) const
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(priority <= MaxPriority);
	return fHeads[priority];
}


RUN_QUEUE_TEMPLATE_LIST
const uint32*
RUN_QUEUE_CLASS_NAME::GetBitmap() const
{
	return fBitmap;
}


RUN_QUEUE_TEMPLATE_LIST
typename RUN_QUEUE_CLASS_NAME::ConstIterator
RUN_QUEUE_CLASS_NAME::GetConstIterator() const
{
	return ConstIterator(this);
}


RUN_QUEUE_TEMPLATE_LIST
Element*
RUN_QUEUE_CLASS_NAME::PeekBest() const
{
	if (fBest != NULL)
		return fBest;

	// Strict priority: only look at the highest priority queue that has threads.
	for (int i = kBitmapSize - 1; i >= 0; i--) {
		uint32 val = fBitmap[i];
		if (val != 0) {
			if (i == kBitmapSize - 1 && (MaxPriority % 32 != 31)) {
				val &= (1UL << (MaxPriority % 32 + 1)) - 1;
				if (val == 0)
					continue;
			}

			int bit = fls(val) - 1;
			unsigned int priority = i * 32 + bit;
			Element* current = fHeads[priority];

			// We found the highest priority. Now find the best candidate
			// strictly within this priority level.
			//
			// Adaptive/Dynamic Search Depth:
			// Scan up to 32 items to find the best candidate (lowest virtual runtime).
			// This mimics "queue" behavior for short lists and "tree-like"
			// fairness for deep lists without the overhead of an actual tree.
			const int kSearchDepth = 32;

			Element* best = current;
			current = sGetLink(current)->fNext;

			for (int j = 1; j < kSearchDepth && current != NULL; j++) {
				if (sCompare(current, best))
					best = current;

				current = sGetLink(current)->fNext;
			}
			fBest = best;
			return best;
		}
	}
	return NULL;
}


RUN_QUEUE_TEMPLATE_LIST
template<typename Predicate>
Element*
RUN_QUEUE_CLASS_NAME::PeekOption(const Predicate& predicate) const
{
	SCHEDULER_ENTER_FUNCTION();

	const int kMaxSearchCount = 16;
	int count = 0;

	for (int i = kBitmapSize - 1; i >= 0; i--) {
		uint32 val = fBitmap[i];

		if (i == kBitmapSize - 1 && (MaxPriority % 32 != 31))
			val &= (1UL << (MaxPriority % 32 + 1)) - 1;

		while (val != 0) {
			int bit = fls(val) - 1;
			val &= ~(1UL << bit);

			unsigned int priority = i * 32 + bit;
			Element* current = fHeads[priority];

			while (current != NULL && count++ < kMaxSearchCount) {
				if (predicate(current))
					return current;

				current = sGetLink(current)->fNext;
			}

			if (count >= kMaxSearchCount)
				return NULL;
		}
	}
	return NULL;
}


RUN_QUEUE_TEMPLATE_LIST
GetLink RUN_QUEUE_CLASS_NAME::sGetLink;

RUN_QUEUE_TEMPLATE_LIST
Compare RUN_QUEUE_CLASS_NAME::sCompare;

RUN_QUEUE_TEMPLATE_LIST
GetLink RUN_QUEUE_CLASS_NAME::ConstIterator::sGetLink;


#endif	// RUN_QUEUE_H
