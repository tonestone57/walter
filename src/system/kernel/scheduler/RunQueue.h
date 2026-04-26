/*
 * Copyright 2013 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 * Audit fixes applied 2025.
 *
 * Authors:
 *		Paweł Dziepak, pdziepak@quarnos.org
 */
#ifndef RUN_QUEUE_H
#define RUN_QUEUE_H


#include <util/BitUtils.h>
#include <util/atomic.h>

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
	static const int kBitmapSize = (MaxPriority + 32) / 32;

	inline	bool		IsEmpty() const { return fTotalCount == 0; }

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
	inline	void		PushBack(Element* element, unsigned int priority);

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

	template<typename Compare2, typename Predicate>
	Element*	PeekBest(const Compare2& compare, const Predicate& predicate) const;

private:
			status_t	fInitStatus;

			uint32		fBitmap[kBitmapSize];

			Element*	fHeads[MaxPriority + 1];
			Element*	fTails[MaxPriority + 1];

	mutable	Element*	fBest;

			int32		fTotalCount;

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

	// (clarification): fPriority is unsigned int.  The guard
	//   if (fPriority == 0) { fNext = NULL; return; }
	// at the top of _FindNextPriority prevents the subtraction (fPriority - 1)
	// from wrapping.  The subsequent topBit = (fPriority - 1) % 32 is
	// therefore always in [0,31] and the 2ULL-shift pattern is safe.
	// The unsigned arithmetic is correct as documented; no code change needed.

	// Nothing exists below priority 0.
	if (fPriority == 0) {
		fNext = NULL;
		return;
	}

	const uint32* bitmap = fList->GetBitmap();

	// Highest priority we may consider is (fPriority - 1), which lives in
	// word 'i' at bit 'topBit'.
	int i = (int)(fPriority - 1) / 32;
	int topBit = (int)(fPriority - 1) % 32;  // 0..31

	// Keep only bits 0..topBit in the starting word.  We use a 64-bit
	// literal (2ULL << topBit) instead of (1UL << (topBit + 1)) to avoid
	// undefined behaviour when topBit == 31: shifting a 32-bit value by
	// 32 is UB on 32-bit targets even though the old guard (currentBit != 31)
	// prevented it from being evaluated, because the guard itself was fragile.
	uint32 val = bitmap[i] & (uint32)((2ULL << topBit) - 1);

	while (true) {
		if (val != 0) {
			int bit = fls(val) - 1;
			fPriority = (unsigned int)(i * 32 + bit);
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
	fBest(NULL),
	fTotalCount(0)
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
		uint32 val = atomic_get((int32*)&fBitmap[i]);
		if (val != 0) {
			if (i == kBitmapSize - 1)
				val &= (uint32)((2ULL << (MaxPriority % 32)) - 1);

			if (val == 0)
				continue;

			int bit = fls(val) - 1;
			unsigned int priority = i * 32 + bit;

			ASSERT(priority <= MaxPriority);
#if B_HAIKU_64_BIT
			Element* head = (Element*)atomic_get64((int64*)&fHeads[priority]);
#else
			Element* head = (Element*)atomic_get((int32*)&fHeads[priority]);
#endif
			ASSERT(head != NULL);
			return head;
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

	// Issue 10/24 fix: capture isEmpty before the bitmap update.
	// Since we hold the run-queue spinlock, this check is atomic
	// with the subsequent insertion.
	bool isEmpty = (fTotalCount == 0);

	fTotalCount++;

	elementLink->fPriority = priority;
	elementLink->fNext = fHeads[priority];
	if (fHeads[priority] != NULL)
		sGetLink(fHeads[priority])->fPrevious = element;
	else {
		fTails[priority] = element;
		fBitmap[priority / 32] |= (1UL << (priority % 32));
	}
	fHeads[priority] = element;

	Element* best = (Element*)atomic_pointer_get((void**)&fBest);
	if (best != NULL) {
		unsigned int bestPriority = sGetLink(best)->fPriority;
		if (priority > bestPriority)
			atomic_pointer_set((void**)&fBest, element);
		else if (priority == bestPriority && sCompare(element, best))
			atomic_pointer_set((void**)&fBest, element);
	} else if (isEmpty || PeekMaximum() == element) {
		atomic_pointer_set((void**)&fBest, element);
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

	// Issue 10/24 fix: capture isEmpty before the bitmap update.
	bool isEmpty = (fTotalCount == 0);

	fTotalCount++;

	elementLink->fPriority = priority;
	elementLink->fPrevious = fTails[priority];
	if (fTails[priority] != NULL)
		sGetLink(fTails[priority])->fNext = element;
	else {
		fHeads[priority] = element;
		fBitmap[priority / 32] |= (1UL << (priority % 32));
	}
	fTails[priority] = element;

	Element* best = (Element*)atomic_pointer_get((void**)&fBest);
	if (best != NULL) {
		unsigned int bestPriority = sGetLink(best)->fPriority;
		if (priority > bestPriority)
			atomic_pointer_set((void**)&fBest, element);
		else if (priority == bestPriority && sCompare(element, best))
			atomic_pointer_set((void**)&fBest, element);
	} else if (isEmpty || PeekMaximum() == element) {
		atomic_pointer_set((void**)&fBest, element);
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

	fTotalCount--;

	elementLink->fPrevious = NULL;
	elementLink->fNext = NULL;

	// Issue 1 fix: when clearing fBest, also clear if the element's
	// priority level is now empty and fBest points to any element in that
	// level (not just the exact element pointer). This handles the case
	// where priority changes leave fBest pointing to a stale entry at a
	// vacated priority bucket.

	// Clear fBest only when it points to the removed element.  Remove is
	// always called under the run queue spinlock; PeekBest is also always
	// called under that same lock.  Within a single lock scope, freed memory
	// the ABA concern about fBest is
	// bounded because ThreadData objects are recycled through an object
	// cache.  Within a single run-queue lock scope no other CPU can
	// allocate and re-enqueue a ThreadData at the same address: allocation
	// requires the object cache lock, and enqueueing requires the run-queue
	// lock we currently hold.  Therefore a stale fBest pointer (pointing
	// to a removed-but-not-yet-freed element) is impossible within the
	// lock scope.  The lockless fast path in PeekBest (no-arg) reads fBest
	// atomically and is protected by the same argument.  No code change
	// required.
	// cannot be reallocated and re-enqueued (allocation requires additional
	// locks), so ABA is impossible.  Unconditional clearing forced a full
	// O(kDeadlineLookaheadLevels * kSearchDepth) rescan on every subsequent
	// PeekBest call regardless of whether the removed element was fBest.
	if ((Element*)atomic_pointer_get((void**)&fBest) == element)
		atomic_pointer_set((void**)&fBest, (Element*)NULL);
	// Additionally clear fBest if its cached priority bucket is now empty,
	// catching the stale-priority-level case.
	else {
		Element* best = (Element*)atomic_pointer_get((void**)&fBest);
		if (best != NULL) {
			RunQueueLink<Element>* bestLink = sGetLink(best);
			unsigned int bestPrio = bestLink->fPriority;
			if (fHeads[bestPrio] == NULL) {
				// The priority bucket fBest was in is now empty; invalidate.
				atomic_pointer_test_and_set((void**)&fBest, (Element*)NULL, best);
			}
		}
	}
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
	Element* bestCandidate = (Element*)atomic_pointer_get((void**)&fBest);
	if (bestCandidate != NULL)
	// Issue 1 fix: validate that fBest is still actually in a non-empty
	// priority bucket before trusting it. A priority change followed by a
	// Remove can leave fBest pointing to an element whose bucket is empty,
	// causing PeekBest to return a stale/dangling entry.
	{
		RunQueueLink<Element>* bestLink = sGetLink(bestCandidate);
		unsigned int bestPrio = bestLink->fPriority;
		// If the bucket is empty the pointer is stale; fall through to rescan.
		if (fHeads[bestPrio] != NULL)
		return bestCandidate;
		// Invalidate stale cache and rescan.
		atomic_pointer_test_and_set((void**)&fBest, (Element*)NULL, bestCandidate);
	}

	// search up to kDeadlineLookaheadLevels non-empty priority
	// levels (highest first) so a lower-priority thread with an earlier virtual
	// deadline can preempt when the top level has no advantage.  The bound
	// keeps worst-case complexity O(kDeadlineLookaheadLevels * kSearchDepth).
	// Issue 11 fix: PeekBest uses its own lookahead logic; PeekOption's
	// shared budget fix does not apply here.
	const int kDeadlineLookaheadLevels = 3;
	int levelsSearched = 0;
	Element* globalBest = NULL;

	for (int i = kBitmapSize - 1; i >= 0 && levelsSearched < kDeadlineLookaheadLevels; i--) {
		uint32 val = fBitmap[i];
		if (val != 0) {
			if (i == kBitmapSize - 1)
				val &= (uint32)((2ULL << (MaxPriority % 32)) - 1);

			if (val == 0)
				continue;

			while (val != 0 && levelsSearched < kDeadlineLookaheadLevels) {
				int bit = fls(val) - 1;
				val &= ~(1UL << bit);
				unsigned int priority = i * 32 + bit;
				Element* current = fHeads[priority];
				if (current == NULL)
					continue;

				// We found a non-empty priority level. Now find the best candidate
				// strictly within this priority level.
				const int kSearchDepth = 32;
				Element* best = current;
				current = sGetLink(current)->fNext;

				for (int j = 1; j < kSearchDepth && current != NULL; j++) {
					if (sCompare(current, best))
						best = current;
					current = sGetLink(current)->fNext;
				}

				if (globalBest == NULL || sCompare(best, globalBest))
					globalBest = best;

				levelsSearched++;
			}
		}
	}

	if (globalBest != NULL)
		atomic_pointer_test_and_set((void**)&fBest, globalBest, (Element*)NULL);

	return globalBest;
}


RUN_QUEUE_TEMPLATE_LIST
template<typename Compare2, typename Predicate>
Element*
RUN_QUEUE_CLASS_NAME::PeekBest(const Compare2& compare, const Predicate& predicate) const
{
	SCHEDULER_ENTER_FUNCTION();

	for (int i = kBitmapSize - 1; i >= 0; i--) {
		uint32 val = fBitmap[i];
		if (val != 0) {
			if (i == kBitmapSize - 1)
				val &= (uint32)((2ULL << (MaxPriority % 32)) - 1);

			if (val == 0)
				continue;

			while (val != 0) {
				int bit = fls(val) - 1;
				val &= ~(1UL << bit);

				unsigned int priority = i * 32 + bit;
				Element* current = fHeads[priority];

				const int kSearchDepth = 32;
				Element* best = NULL;

				for (int j = 0; j < kSearchDepth && current != NULL; j++) {
					if (predicate(current)) {
						if (best == NULL || compare(current, best))
							best = current;
					}

					current = sGetLink(current)->fNext;
				}

				if (best != NULL)
					return best;
			}
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

	// Scale search depth based on system size.
	// More cores = more budget to find better affinity.
	const int kNumCPUs = smp_get_num_cpus();
	const int kMaxSearchPerLevel = 16 + (kNumCPUs >> 3);

	// Issue 11 fix: Increase totalBudget to allow searching more priority
	// levels before giving up.  kMaxSearchPerLevel * 8 allows up to 8
	// full levels or many partially-occupied levels.
	int totalBudget = kMaxSearchPerLevel * 8;

	for (int i = kBitmapSize - 1; i >= 0; i--) {
		uint32 val = fBitmap[i];

		if (i == kBitmapSize - 1)
			val &= (uint32)((2ULL << (MaxPriority % 32)) - 1);

		while (val != 0) {
			int bit = fls(val) - 1;
			val &= ~(1UL << bit);

			unsigned int priority = i * 32 + bit;
			Element* current = fHeads[priority];
			int count = 0;

			// Give each priority level a fair, equal share of the
			// total budget.  The previous "/ 2 + 1" formula halved the budget
			// at every level, causing the second priority band to receive only
			// half as many probes as the first.  This under-served lower-
			// priority stealable threads and made work-stealing incomplete.
			int searchLimit = min_c(kMaxSearchPerLevel, totalBudget);
			while (current != NULL && count++ < searchLimit) {
				if (predicate(current))
					return current;

				current = sGetLink(current)->fNext;
				// Issue 22 fix: decrement budget here, paired with element
				// visitation, so the per-level cap and budget cap are both
				// correctly accounted for in the same decrement.
				totalBudget--;
			}

			// 'break' only exits the inner while(val!=0) loop.
			// Return NULL to terminate the outer for-loop immediately when
			// the budget is exhausted — remaining bitmap words are skipped.
			if (totalBudget <= 0)
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
