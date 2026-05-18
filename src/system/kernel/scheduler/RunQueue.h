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
#if SCHEDULER_MASK_IS_64_BIT
static const int32 kPrimaryBins = 64;
static const int32 kSecondaryBins = 64;
static const int32 kSecondaryBinsShift = 6;
#else
static const int32 kPrimaryBins = 32;
static const int32 kSecondaryBins = 32;
static const int32 kSecondaryBinsShift = 5;
#endif

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

	inline bool IsEmpty() const { return atomic_get((int32 volatile*)&fTotalCount) == 0; }

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

	inline native_cpu_mask_t GetFirstLevelBitmap() const
	{
		return cpu_mask_get_atomic(const_cast<native_cpu_mask_t*>(&fFirstLevelBitmap));
	}
	inline native_cpu_mask_t GetSecondLevelBitmap(int fli) const
	{
		return cpu_mask_get_atomic(const_cast<native_cpu_mask_t*>(&fSecondLevelBitmap[fli]));
	}
	inline uint32 GetRealTimeBitmap() const
	{
		return (uint32)atomic_get((int32 volatile*)&fRealTimeBitmap);
	}

	inline bool TestAndClearSliAtomic(int fli, int sli)
	{
		native_cpu_mask_t bit = (native_cpu_mask_t)1 << sli;
		native_cpu_mask_t old = cpu_mask_and_atomic(&fSecondLevelBitmap[fli], ~bit);
		return (old & bit) != 0;
	}

	inline bool TestAndClearRTAtomic(int index)
	{
		uint32 bit = 1U << index;
		uint32 old = (uint32)atomic_and((int32 volatile*)&fRealTimeBitmap, (int32)~bit);
		return (old & bit) != 0;
	}

	inline void RestoreRTBitAtomic(int index)
	{
		atomic_or((int32 volatile*)&fRealTimeBitmap, (int32)(1U << index));
	}

	inline void RestoreSliBitAtomic(int fli, int sli)
	{
		cpu_mask_or_atomic(&fSecondLevelBitmap[fli], (native_cpu_mask_t)1 << sli);
		cpu_mask_or_atomic(&fFirstLevelBitmap, (native_cpu_mask_t)1 << fli);
	}

	inline Element* GetRTBinHead(int index) const { return fRealTimeQueues[index].Head(); }
	inline bool IsRTBinEmpty(int index) const { return fRealTimeQueues[index].IsEmpty(); }
	inline Element* GetNextRT(Element* element, int index) const { return fRealTimeQueues[index].GetNext(element); }

	inline Element* GetSliBinHead(int fli, int sli) const { return fQueues[fli][sli].Head(); }
	inline bool IsSliBinEmpty(int fli, int sli) const { return fQueues[fli][sli].IsEmpty(); }
	inline Element* GetNextSli(Element* element, int fli, int sli) const { return fQueues[fli][sli].GetNext(element); }

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

	native_cpu_mask_t fFirstLevelBitmap;
	native_cpu_mask_t fSecondLevelBitmap[kPrimaryBins];
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
#if SCHEDULER_MASK_IS_64_BIT
	native_cpu_mask_t dnative = (native_cpu_mask_t)delta;
#else
	native_cpu_mask_t dnative = (native_cpu_mask_t)min_c((bigtime_t)0xFFFFFFFF, delta);
#endif
	fli = scheduler_flsnative(dnative) - 1;
	if (fli >= kPrimaryBins) fli = kPrimaryBins - 1;
	if (fli < 0) fli = 0;

	// Linearly divide the space between 2^fli and 2^(fli+1)
	if (fli < kSecondaryBinsShift)
		sli = (dnative - ((native_cpu_mask_t)1 << fli)) << (kSecondaryBinsShift - fli);
	else
		sli = (dnative - ((native_cpu_mask_t)1 << fli)) >> (fli - kSecondaryBinsShift);
	sli &= (kSecondaryBins - 1);
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::PushBack(Element* element, unsigned int priority, bigtime_t svt)
{
	if (IsEmpty())
		fSystemVirtualTime = svt;
	Thread* thread = element->GetThread();

	Traits::SetInRunQueue(element, true);
	atomic_add((int32 volatile*)&fTotalCount, 1);

	if (priority >= 100) {
		int32 index = priority - 100;
		if (index < 0) index = 0;
		if (index > 20) index = 20;
		fRealTimeQueues[index].Add(element);
		atomic_or((int32 volatile*)&fRealTimeBitmap, (int32)(1U << index));
		thread->fli_index = -1; // Mark as RT
		thread->sli_index = index;
	} else {
		int32 fli, sli;
		_GetIndices(thread->virtual_deadline, fli, sli);
		thread->fli_index = fli;
		thread->sli_index = sli;

		fQueues[fli][sli].Add(element);
		cpu_mask_or_atomic(&fSecondLevelBitmap[fli], (native_cpu_mask_t)1 << sli);
		cpu_mask_or_atomic(&fFirstLevelBitmap, (native_cpu_mask_t)1 << fli);
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
	atomic_add((int32 volatile*)&fTotalCount, 1);

	if (priority >= 100) {
		int32 index = priority - 100;
		if (index < 0) index = 0;
		if (index > 20) index = 20;
		fRealTimeQueues[index].Add(element, false); // Add at head
		atomic_or((int32 volatile*)&fRealTimeBitmap, (int32)(1U << index));
		thread->fli_index = -1; // Mark as RT
		thread->sli_index = index;
	} else {
		int32 fli, sli;
		_GetIndices(thread->virtual_deadline, fli, sli);
		thread->fli_index = fli;
		thread->sli_index = sli;

		fQueues[fli][sli].Add(element, false); // Add at head
		cpu_mask_or_atomic(&fSecondLevelBitmap[fli], (native_cpu_mask_t)1 << sli);
		cpu_mask_or_atomic(&fFirstLevelBitmap, (native_cpu_mask_t)1 << fli);
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
			atomic_and((int32 volatile*)&fRealTimeBitmap, (int32)~(1U << index));
	} else {
		int32 fli = thread->fli_index;
		int32 sli = thread->sli_index;

		fQueues[fli][sli].Remove(element);
		if (fQueues[fli][sli].IsEmpty()) {
			cpu_mask_and_atomic(&fSecondLevelBitmap[fli], ~((native_cpu_mask_t)1 << sli));
			if (cpu_mask_get_atomic(&fSecondLevelBitmap[fli]) == 0)
				cpu_mask_and_atomic(&fFirstLevelBitmap, ~((native_cpu_mask_t)1 << fli));
		}
	}

	Traits::SetInRunQueue(element, false);
	atomic_add((int32 volatile*)&fTotalCount, -1);
}

RUN_QUEUE_TEMPLATE_LIST
Element* RUN_QUEUE_CLASS_NAME::PeekBest() const
{
	uint32 rtBitmap = GetRealTimeBitmap();
	if (rtBitmap != 0) {
		int32 index = fls(rtBitmap) - 1;
		if (index >= 0)
			return fRealTimeQueues[index].Head();
	}

	native_cpu_mask_t flBitmap = GetFirstLevelBitmap();
	if (flBitmap == 0) return NULL;

	int32 fli = scheduler_ctz(flBitmap);
	if (fli < 0) return NULL;

	native_cpu_mask_t slBitmap = GetSecondLevelBitmap(fli);
	if (slBitmap == 0) return NULL;

	int32 sli = scheduler_ctz(slBitmap);
	if (sli < 0) return NULL;

	return fQueues[fli][sli].Head();
}

RUN_QUEUE_TEMPLATE_LIST
template <typename Compare2, typename Predicate>
Element* RUN_QUEUE_CLASS_NAME::PeekBest(const Compare2& compare,
										const Predicate& predicate) const
{
	Element* best = NULL;

	// Check Real-Time queues
	uint32 rtBitmap = GetRealTimeBitmap();
	while (rtBitmap != 0) {
		int32 index = fls(rtBitmap) - 1;
		if (index >= 0) {
			typename DoublyLinkedList<Element, GetLink>::Iterator it = const_cast<DoublyLinkedList<Element, GetLink>&>(fRealTimeQueues[index]).GetIterator();
			while (it.HasNext()) {
				Element* element = it.Next();
				if (predicate(element)) {
					if (best == NULL || compare(element, best))
						best = element;
				}
			}
		}
		rtBitmap &= ~(1U << index);
	}

	// Check EEVDF matrix
	native_cpu_mask_t flBitmap = GetFirstLevelBitmap();
	while (flBitmap != 0) {
		int32 fli = scheduler_ctz(flBitmap);
		if (fli < 0) break;
		native_cpu_mask_t slBitmap = GetSecondLevelBitmap(fli);
		while (slBitmap != 0) {
			int32 sli = scheduler_ctz(slBitmap);
			if (sli < 0) break;
			typename DoublyLinkedList<Element, GetLink>::Iterator it = const_cast<DoublyLinkedList<Element, GetLink>&>(fQueues[fli][sli]).GetIterator();
			while (it.HasNext()) {
				Element* element = it.Next();
				if (predicate(element)) {
					if (best == NULL || compare(element, best))
						best = element;
				}
			}
			slBitmap &= ~((native_cpu_mask_t)1 << sli);
		}
		flBitmap &= ~((native_cpu_mask_t)1 << fli);
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
