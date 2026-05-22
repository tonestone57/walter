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

template <typename Element>
class RunQueueStandardGetLink {
public:
	inline DoublyLinkedListLink<Element>* operator()(Element* element) const
	{
		return element->GetRunQueueLink();
	}
};

namespace Scheduler {

// 512-lane optimized flat run-queue constants
static const int32 kPrimaryBins = 16;
static const int32 kSecondaryBins = 32;
static const int32 kNumLanes = 512;
static const int32 kRTSubsystemSignal = 15;

/*
 * Haiku OS non-realtime priority mapping table for a 512-lane EEVDF matrix.
 * Maps priority values 0-99 to priority bands 0-15.
 */
static const uint8 kPriorityToRowMap[100] = {
/* 0 - 9 */   0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 10 - 19 */ 2, 2, 2, 2, 2, 3, 3, 3, 4, 4,
/* 20 - 29 */ 5, 6, 6, 7, 7, 8, 8, 8, 9, 9,
/* 30 - 39 */ 10, 11, 11, 11, 11, 12, 12, 12, 12, 12,
/* 40 - 49 */ 13, 14, 14, 14, 14, 14, 14, 14, 14, 14,
/* 50 - 59 */ 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
/* 60 - 69 */ 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
/* 70 - 79 */ 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
/* 80 - 89 */ 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
/* 90 - 99 */ 15, 15, 15, 15, 15, 15, 15, 15, 15, 15
};

inline uint8 GetSchedulerMatrixRow(int32 priority) {
	if (unlikely(priority < 0)) return 0;
	if (unlikely(priority >= 100)) return kRTSubsystemSignal;
	return kPriorityToRowMap[priority];
}

template <typename Element>
struct RunQueueTraits {
	static inline void SetInRunQueue(Element* element, bool inQueue) {}
};

class CPUEntry;
class CoreEntry;

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

	inline bool IsEmpty() const { return LoadAcquire(fTotalCount) == 0; }

	void PushBack(Element* element, unsigned int priority, bigtime_t svt = 0);
	void PushFront(Element* element, unsigned int priority, bigtime_t svt = 0);
	void Remove(Element* element);

	Element* PeekBest() const;
	template <typename Compare2, typename Predicate>
	Element* PeekBest(const Compare2& compare,
					  const Predicate& predicate) const;
	Element* PopNext();

	inline status_t CheckCapacity(int32 count) { return B_OK; }
	void CheckEligibility(bigtime_t svt);
	inline Element* PeekRoot() const { return PeekBest(); }
	inline Element* PeekMaximum() const { return PeekBest(); }

	// Optimized flat bitmask accessors
	inline uint32 GetRealTimeBitmap() const
	{
		return (uint32)LoadAcquire(fRealTimeBitmap);
	}

	inline bool TestAndClearRTAtomic(int index)
	{
		uint32 bit = 1U << index;
		uint32 old = (uint32)AndAtomic(fRealTimeBitmap, (int32)~bit);
		return (old & bit) != 0;
	}

	inline void RestoreRTBitAtomic(int index)
	{
		OrAtomic(fRealTimeBitmap, (int32)(1U << index));
	}

	// Lane-based atomic helpers for decentralized stealing
	inline bool TestAndClearLaneAtomic(int lane)
	{
		int word = lane / (sizeof(native_cpu_mask_t) * 8);
		int bit = lane % (sizeof(native_cpu_mask_t) * 8);
		native_cpu_mask_t mask = (native_cpu_mask_t)1 << bit;
		native_cpu_mask_t old = cpu_mask_and_atomic(&fBitmap[word], ~mask);
		return (old & mask) != 0;
	}

	inline void RestoreLaneBitAtomic(int lane)
	{
		int word = lane / (sizeof(native_cpu_mask_t) * 8);
		int bit = lane % (sizeof(native_cpu_mask_t) * 8);
		cpu_mask_or_atomic(&fBitmap[word], (native_cpu_mask_t)1 << bit);
	}

	inline Element* GetRTBinHead(int index) const { return fRealTimeQueues[index].Head(); }
	inline bool IsRTBinEmpty(int index) const { return fRealTimeQueues[index].IsEmpty(); }
	inline Element* GetNextRT(Element* element, int index) const { return fRealTimeQueues[index].GetNext(element); }

	inline Element* GetLaneBinHead(int lane) const { return fQueues[lane].Head(); }
	inline bool IsLaneBinEmpty(int lane) const { return fQueues[lane].IsEmpty(); }
	inline Element* GetNextLane(Element* element, int lane) const { return fQueues[lane].GetNext(element); }

	class ConstIterator {
	public:
		ConstIterator() : fQueue(NULL), fBand(kPrimaryBins - 1), fSLI(0), fRT(20), fCurrent(NULL) {}
		ConstIterator(const RunQueue* queue) : fQueue(queue), fBand(kPrimaryBins - 1), fSLI(0), fRT(20), fCurrent(NULL)
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
				if (fRT >= 0) {
					fCurrent = fQueue->fRealTimeQueues[fRT].GetNext(fCurrent);
					if (fCurrent != NULL) return;
					fRT--;
				} else {
					fCurrent = fQueue->fQueues[fBand * 32 + fSLI].GetNext(fCurrent);
					if (fCurrent != NULL) return;

					fSLI++;
					if (fSLI >= 32) {
						fSLI = 0;
						fBand--;
					}
					_AdvanceLane();
					return;
				}
			}

			while (fRT >= 0) {
				fCurrent = fQueue->fRealTimeQueues[fRT].Head();
				if (fCurrent != NULL) return;
				fRT--;
			}

			_AdvanceLane();
		}

		void _AdvanceLane()
		{
			while (fBand >= 0) {
				while (fSLI < 32) {
					fCurrent = fQueue->fQueues[fBand * 32 + fSLI].Head();
					if (fCurrent != NULL) return;
					fSLI++;
				}
				fSLI = 0;
				fBand--;
			}
			fCurrent = NULL;
		}

		const RunQueue* fQueue;
		int fBand, fSLI, fRT;
		Element* fCurrent;
	};

	inline ConstIterator GetConstIterator() const { return ConstIterator(this); }

	Element* GetHead(unsigned int priority) const;

private:
	inline int32 _GetLane(bigtime_t deadline, int32 priority) const;

	// Flat 512-bit mask optimized for single-cache-line footprint (64 bytes).
	// On 64-bit: 8 words of 64 bits. On 32-bit: 16 words of 32 bits.
	static const int kNumWords = kNumLanes / (sizeof(native_cpu_mask_t) * 8);
	native_cpu_mask_t fBitmap[kNumWords] __attribute__((aligned(64)));

	DoublyLinkedList<Element, GetLink> fQueues[kNumLanes] __attribute__((aligned(64)));

	uint32 fRealTimeBitmap;
	DoublyLinkedList<Element, GetLink> fRealTimeQueues[21];

	bigtime_t fSystemVirtualTime;
	int32 fTotalCount;

	static GetLink sGetLink;

	// Friends for direct access in work-stealing
	friend class CPUEntry;
	friend class CoreEntry;
};

RUN_QUEUE_TEMPLATE_LIST
RUN_QUEUE_CLASS_NAME::RunQueue()
	: fRealTimeBitmap(0),
	  fSystemVirtualTime(0),
	  fTotalCount(0)
{
	memset(const_cast<native_cpu_mask_t*>(fBitmap), 0, sizeof(fBitmap));
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::CheckEligibility(bigtime_t svt)
{
	if (IsEmpty())
		fSystemVirtualTime = svt;
}

RUN_QUEUE_TEMPLATE_LIST
int32 RUN_QUEUE_CLASS_NAME::_GetLane(bigtime_t deadline, int32 priority) const
{
	int32 fli = (int32)GetSchedulerMatrixRow(priority);

	bigtime_t delta = deadline - fSystemVirtualTime;
	int32 sli = 0;
	if (delta > 0) {
		int64 bucketSize = LoadAcquire64(gDeadlineBucketSize);
		if (bucketSize <= 0) bucketSize = 5000000;
		sli = (int32)(delta / bucketSize);
		if (sli >= kSecondaryBins) sli = kSecondaryBins - 1;
	}

	return fli * 32 + sli;
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::PushBack(Element* element, unsigned int priority, bigtime_t svt)
{
	if (IsEmpty())
		fSystemVirtualTime = svt;
	Thread* thread = element->GetThread();

	Traits::SetInRunQueue(element, true);
	AddRelease(fTotalCount, 1);

	if (priority >= 100) {
		int32 index = (int32)priority - 100;
		if (index < 0) index = 0;
		if (index > 20) index = 20;
		fRealTimeQueues[index].Add(element);
		OrAtomic(fRealTimeBitmap, (int32)(1U << index));
		thread->fli_index = -1;
		thread->sli_index = index;
	} else {
		int32 lane = _GetLane(thread->virtual_deadline, (int32)priority);
		thread->fli_index = lane; // We reuse fli_index to store flat lane

		fQueues[lane].Add(element);

		int word = lane / (sizeof(native_cpu_mask_t) * 8);
		int bit = lane % (sizeof(native_cpu_mask_t) * 8);
		cpu_mask_or_atomic(&fBitmap[word], (native_cpu_mask_t)1 << bit);
	}
}

RUN_QUEUE_TEMPLATE_LIST
void RUN_QUEUE_CLASS_NAME::PushFront(Element* element, unsigned int priority, bigtime_t svt)
{
	if (IsEmpty())
		fSystemVirtualTime = svt;
	Thread* thread = element->GetThread();

	Traits::SetInRunQueue(element, true);
	AddRelease(fTotalCount, 1);

	if (priority >= 100) {
		int32 index = (int32)priority - 100;
		if (index < 0) index = 0;
		if (index > 20) index = 20;
		fRealTimeQueues[index].Add(element, false);
		OrAtomic(fRealTimeBitmap, (int32)(1U << index));
		thread->fli_index = -1;
		thread->sli_index = index;
	} else {
		int32 lane = _GetLane(thread->virtual_deadline, (int32)priority);
		thread->fli_index = lane;

		fQueues[lane].Add(element, false);

		int word = lane / (sizeof(native_cpu_mask_t) * 8);
		int bit = lane % (sizeof(native_cpu_mask_t) * 8);
		cpu_mask_or_atomic(&fBitmap[word], (native_cpu_mask_t)1 << bit);
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
			AndAtomic(fRealTimeBitmap, (int32)~(1U << index));
	} else {
		int32 lane = thread->fli_index;
		fQueues[lane].Remove(element);
		if (fQueues[lane].IsEmpty()) {
			int word = lane / (sizeof(native_cpu_mask_t) * 8);
			int bit = lane % (sizeof(native_cpu_mask_t) * 8);
			cpu_mask_and_atomic(&fBitmap[word], ~((native_cpu_mask_t)1 << bit));
		}
	}

	Traits::SetInRunQueue(element, false);
	AddRelease(fTotalCount, -1);
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

	// Optimized O(1) word-by-word scan for 512 lanes.
	// Preserves "Highest Band Wins, then Lowest Deadline (in band) Wins".
	// idx = fli * 32 + sli.
	for (int band = kPrimaryBins - 1; band >= 0; band--) {
		// Calculate the range of bits for this band.
		// Each band is exactly 32 bits (kSecondaryBins).
		int startLane = band * 32;

		// Map the 32-bit band to the flat bitmask.
		// On 32-bit systems, a band maps exactly to one word.
		// On 64-bit systems, two bands map to one word.
		int wordIdx = startLane / (sizeof(native_cpu_mask_t) * 8);
		int bitOffset = startLane % (sizeof(native_cpu_mask_t) * 8);

		native_cpu_mask_t word = cpu_mask_get_atomic(&fBitmap[wordIdx]);
		if (word == 0) continue;

		// Mask the word to isolate only the 32 bits belonging to this band.
		native_cpu_mask_t bandMask = (native_cpu_mask_t)0xFFFFFFFF << bitOffset;
		native_cpu_mask_t isolatedBand = word & bandMask;

		if (isolatedBand != 0) {
			// Find the lowest set bit (CTZ) within this band to get the lowest deadline.
			int bit = scheduler_ctz(isolatedBand);
			return fQueues[wordIdx * (sizeof(native_cpu_mask_t) * 8) + bit].Head();
		}
	}

	return NULL;
}

RUN_QUEUE_TEMPLATE_LIST
template <typename Compare2, typename Predicate>
Element* RUN_QUEUE_CLASS_NAME::PeekBest(const Compare2& compare,
										const Predicate& predicate) const
{
	Element* best = NULL;

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

	// Optimized O(1) band-by-band scan for template PeekBest.
	// Scans all bands because compare(element, best) might prefer a thread
	// in a lower priority band.
	for (int band = kPrimaryBins - 1; band >= 0; band--) {
		int startLane = band * 32;
		int wordIdx = startLane / (sizeof(native_cpu_mask_t) * 8);
		int bitOffset = startLane % (sizeof(native_cpu_mask_t) * 8);

		native_cpu_mask_t word = cpu_mask_get_atomic(&fBitmap[wordIdx]);
		if (word == 0) continue;

		native_cpu_mask_t bandMask = (native_cpu_mask_t)0xFFFFFFFF << bitOffset;
		native_cpu_mask_t isolatedBand = word & bandMask;

		while (isolatedBand != 0) {
			int bit = scheduler_ctz(isolatedBand);
			int lane = wordIdx * (sizeof(native_cpu_mask_t) * 8) + bit;

			typename DoublyLinkedList<Element, GetLink>::Iterator it = const_cast<DoublyLinkedList<Element, GetLink>&>(fQueues[lane]).GetIterator();
			while (it.HasNext()) {
				Element* element = it.Next();
				if (predicate(element)) {
					if (best == NULL || compare(element, best))
						best = element;
				}
			}
			isolatedBand &= ~((native_cpu_mask_t)1 << bit);
		}
	}

	return best;
}

RUN_QUEUE_TEMPLATE_LIST
Element* RUN_QUEUE_CLASS_NAME::GetHead(unsigned int priority) const
{
	if (priority >= 100) {
		int32 index = (int32)priority - 100;
		if (index < 0) index = 0;
		if (index > 20) index = 20;
		return fRealTimeQueues[index].Head();
	}

	if (priority == B_IDLE_PRIORITY) {
		return fQueues[0].Head();
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

}  // namespace Scheduler

#endif	// RUN_QUEUE_H
