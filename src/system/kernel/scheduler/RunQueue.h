/*
 * Copyright 2013-2025 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Paweł Dziepak, pdziepak@quarnos.org
 *      Jules (2025 BMQ-EEVDF implementation)
 */
#ifndef RUN_QUEUE_H
#define RUN_QUEUE_H

#include <util/BitUtils.h>
#include <util/atomic.h>
#include <util/DoublyLinkedList.h>

#include "scheduler_profiler.h"

// For BMQ EEVDF mapping
static const int32 kPrimaryBins = 32;
static const int32 kSecondaryBins = 32;

template <typename Element>
class RunQueueStandardGetLink {
public:
	inline DoublyLinkedListLink<Element>* operator()(Element* element) const
	{
		return element->GetRunQueueLink();
	}
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
	RunQueue();

	inline bool IsEmpty() const { return atomic_get(&fTotalCount) == 0; }

	void PushBack(Element* element, unsigned int priority, bigtime_t svt = 0);
	void PushFront(Element* element, unsigned int priority, bigtime_t svt = 0);
	void Remove(Element* element);

	Element* PeekBest() const;
	template <typename Compare2, typename Predicate>
	Element* PeekBest(const Compare2& compare,
					  const Predicate& predicate) const;
	Element* PopNext();

	// Compatibility methods
	inline status_t CheckCapacity(int32 count) { return B_OK; }
	void CheckEligibility(bigtime_t svt);
	inline Element* PeekRoot() const { return PeekBest(); }
	inline Element* PeekMaximum() const { return PeekBest(); }

	inline uint32 GetFirstLevelBitmap() const { return fFirstLevelBitmap; }
	inline uint32 GetSecondLevelBitmap(int fli) const { return fSecondLevelBitmap[fli]; }
	inline uint32 GetRealTimeBitmap() const { return fRealTimeBitmap; }

	inline Element* GetBinHead(int fli, int sli) const { return fQueues[fli][sli].Head(); }

	class ConstIterator {
	public:
		ConstIterator() : fQueue(NULL), fFLI(0), fSLI(0), fRT(0), fCurrent(NULL) {}
		ConstIterator(const RunQueue* queue) : fQueue(queue), fFLI(0), fSLI(0), fRT(0), fCurrent(NULL)
		{
			_Advance();
		}

		bool HasNext() const { return fCurrent != NULL; }
		Element* Next()
		{
			Element* result = fCurrent;
			_Advance();
			return result;
		}
	private:
		void _Advance()
		{
			if (fQueue == NULL) return;

			if (fCurrent != NULL) {
				if (fRT < 21) {
					fCurrent = fQueue->fRealTimeQueues[fRT].GetNext(fCurrent);
					if (fCurrent != NULL) return;
					fRT++;
				} else {
					fCurrent = fQueue->fQueues[fFLI][fSLI].GetNext(fCurrent);
					if (fCurrent != NULL) return;

					fSLI++;
					if (fSLI >= kSecondaryBins) {
						fSLI = 0;
						fFLI++;
					}
					_AdvanceBin();
					return;
				}
			}

			// Find next non-empty RT queue
			while (fRT < 21) {
				fCurrent = fQueue->fRealTimeQueues[fRT].Head();
				if (fCurrent != NULL) return;
				fRT++;
			}

			// Find next non-empty EEVDF bin
			_AdvanceBin();
		}

		void _AdvanceBin()
		{
			while (fFLI < kPrimaryBins) {
				while (fSLI < kSecondaryBins) {
					fCurrent = fQueue->fQueues[fFLI][fSLI].Head();
					if (fCurrent != NULL) return;
					fSLI++;
				}
				fSLI = 0;
				fFLI++;
			}
			fCurrent = NULL;
		}

		const RunQueue* fQueue;
		int fFLI, fSLI, fRT;
		Element* fCurrent;
	};

	inline ConstIterator GetConstIterator() const { return ConstIterator(this); }

	Element* GetHead(unsigned int priority) const;

private:
	void _GetIndices(bigtime_t deadline, int32& fli, int32& sli) const;

	uint32 fFirstLevelBitmap;
	uint32 fSecondLevelBitmap[kPrimaryBins];
	DoublyLinkedList<Element, GetLink> fQueues[kPrimaryBins][kSecondaryBins];

	uint32 fRealTimeBitmap;
	DoublyLinkedList<Element, GetLink> fRealTimeQueues[21];

	bigtime_t fSystemVirtualTime;
	int32 fTotalCount;

	static GetLink sGetLink;
};

RUN_QUEUE_TEMPLATE_LIST
RUN_QUEUE_CLASS_NAME::RunQueue()
	: fFirstLevelBitmap(0),
	  fRealTimeBitmap(0),
	  fSystemVirtualTime(0),
	  fTotalCount(0)
{
	memset(fSecondLevelBitmap, 0, sizeof(fSecondLevelBitmap));
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::CheckEligibility(bigtime_t svt)
{
	if (IsEmpty())
		fSystemVirtualTime = svt;
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::_GetIndices(bigtime_t deadline, int32& fli, int32& sli) const
{
	// Haiku bigtime_t is in microseconds.
	bigtime_t delta = deadline - fSystemVirtualTime;
	if (delta <= 0) {
		fli = 0;
		sli = 0;
		return;
	}

	// Find the highest power of 2 using Count Leading Zeros instruction
	uint32 d32 = (uint32)min_c((bigtime_t)0xFFFFFFFF, delta);
	fli = fls(d32) - 1;
	if (fli >= kPrimaryBins) fli = kPrimaryBins - 1;

	// Linearly divide the space between 2^fli and 2^(fli+1)
	if (fli < 5) {
		sli = (d32 - (1 << fli)) << (5 - fli);
	} else {
		sli = (d32 - (1 << fli)) >> (fli - 5);
	}
	sli &= 31;
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::PushBack(Element* element, unsigned int priority, bigtime_t svt)
{
	if (IsEmpty())
		fSystemVirtualTime = svt;
	Thread* thread = element->GetThread();

	Traits::SetInRunQueue(element, true);
	atomic_add(&fTotalCount, 1);

	if (priority >= 100) {
		int32 index = priority - 100;
		if (index < 0) index = 0;
		if (index > 20) index = 20;
		fRealTimeQueues[index].Add(element);
		fRealTimeBitmap |= (1 << index);
		thread->fli_index = -1; // Mark as RT
		thread->sli_index = index;
	} else {
		int32 fli, sli;
		_GetIndices(thread->virtual_deadline, fli, sli);
		thread->fli_index = fli;
		thread->sli_index = sli;

		fQueues[fli][sli].Add(element);
		fSecondLevelBitmap[fli] |= (1 << sli);
		fFirstLevelBitmap |= (1 << fli);
	}
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::PushFront(Element* element, unsigned int priority, bigtime_t svt)
{
	// For BMQ EEVDF, PushFront is similar to PushBack since ordering is by deadline,
	// but within the same bin we can put it at the head.
	if (IsEmpty())
		fSystemVirtualTime = svt;
	Thread* thread = element->GetThread();

	Traits::SetInRunQueue(element, true);
	atomic_add(&fTotalCount, 1);

	if (priority >= 100) {
		int32 index = priority - 100;
		if (index < 0) index = 0;
		if (index > 20) index = 20;
		fRealTimeQueues[index].Add(element, false); // Add at head
		fRealTimeBitmap |= (1 << index);
		thread->fli_index = -1; // Mark as RT
		thread->sli_index = index;
	} else {
		int32 fli, sli;
		_GetIndices(thread->virtual_deadline, fli, sli);
		thread->fli_index = fli;
		thread->sli_index = sli;

		fQueues[fli][sli].Add(element, false); // Add at head
		fSecondLevelBitmap[fli] |= (1 << sli);
		fFirstLevelBitmap |= (1 << fli);
	}
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::Remove(Element* element)
{
	Thread* thread = element->GetThread();

	if (thread->fli_index < 0) {
		int32 index = thread->sli_index;

		fRealTimeQueues[index].Remove(element);
		if (fRealTimeQueues[index].IsEmpty())
			fRealTimeBitmap &= ~(1 << index);
	} else {
		int32 fli = thread->fli_index;
		int32 sli = thread->sli_index;

		fQueues[fli][sli].Remove(element);
		if (fQueues[fli][sli].IsEmpty()) {
			fSecondLevelBitmap[fli] &= ~(1 << sli);
			if (fSecondLevelBitmap[fli] == 0)
				fFirstLevelBitmap &= ~(1 << fli);
		}
	}

	Traits::SetInRunQueue(element, false);
	atomic_add(&fTotalCount, -1);
}

RUN_QUEUE_TEMPLATE_LIST
Element* RUN_QUEUE_CLASS_NAME::PeekBest() const
{
	if (fRealTimeBitmap != 0) {
		int32 index = fls(fRealTimeBitmap) - 1;
		return fRealTimeQueues[index].Head();
	}

	if (fFirstLevelBitmap == 0) return NULL;

	int32 fli = ffs(fFirstLevelBitmap) - 1;
	int32 sli = ffs(fSecondLevelBitmap[fli]) - 1;

	return fQueues[fli][sli].Head();
}

RUN_QUEUE_TEMPLATE_LIST
template <typename Compare2, typename Predicate>
Element* RUN_QUEUE_CLASS_NAME::PeekBest(const Compare2& compare,
										const Predicate& predicate) const
{
	Element* best = NULL;

	// Check Real-Time queues
	uint32 rtBitmap = fRealTimeBitmap;
	while (rtBitmap != 0) {
		int32 index = fls(rtBitmap) - 1;
		typename DoublyLinkedList<Element, GetLink>::Iterator it = fRealTimeQueues[index].GetIterator();
		while (it.HasNext()) {
			Element* element = it.Next();
			if (predicate(element)) {
				if (best == NULL || compare(element, best))
					best = element;
			}
		}
		rtBitmap &= ~(1 << index);
	}

	// Check EEVDF matrix
	uint32 flBitmap = fFirstLevelBitmap;
	while (flBitmap != 0) {
		int32 fli = ffs(flBitmap) - 1;
		uint32 slBitmap = fSecondLevelBitmap[fli];
		while (slBitmap != 0) {
			int32 sli = ffs(slBitmap) - 1;
			typename DoublyLinkedList<Element, GetLink>::Iterator it = fQueues[fli][sli].GetIterator();
			while (it.HasNext()) {
				Element* element = it.Next();
				if (predicate(element)) {
					if (best == NULL || compare(element, best))
						best = element;
				}
			}
			slBitmap &= ~(1 << sli);
		}
		flBitmap &= ~(1 << fli);
	}

	return best;
}

RUN_QUEUE_TEMPLATE_LIST
Element* RUN_QUEUE_CLASS_NAME::GetHead(unsigned int priority) const
{
	if (priority >= 100) {
		int32 index = priority - 100;
		if (index < 0) index = 0;
		if (index > 20) index = 20;
		return fRealTimeQueues[index].Head();
	}

	if (priority == B_IDLE_PRIORITY) {
		return fQueues[0][0].Head();
	}

	return NULL;
}

RUN_QUEUE_TEMPLATE_LIST
Element* RUN_QUEUE_CLASS_NAME::PopNext()
{
	Element* element = PeekBest();
	if (element != NULL)
		Remove(element);
	return element;
}

RUN_QUEUE_TEMPLATE_LIST
GetLink RUN_QUEUE_CLASS_NAME::sGetLink;

#endif	// RUN_QUEUE_H
