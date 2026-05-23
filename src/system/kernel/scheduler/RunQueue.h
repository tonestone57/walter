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

	// Real-Time priorities (100-120) handled separately via RR queues
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

	inline Element* GetRTBinHead(int index) const { return fRealTimeQueues[index].Head(); }
	inline bool IsRTBinEmpty(int index) const { return fRealTimeQueues[index].IsEmpty(); }
	inline Element* GetNextRT(Element* element, int index) const { return fRealTimeQueues[index].GetNext(element); }

	// Optimized flat bitmask accessors for FairShare lanes (0-511)
	inline native_cpu_mask_t GetBitmapWord(int index) const
	{
		return cpu_mask_get_atomic(&fBitmap[index]);
	}

	inline bool IsBitmapEmpty() const
	{
		for (int i = 0; i < kNumWords; i++) {
			if (cpu_mask_get_atomic(&fBitmap[i]) != 0)
				return false;
		}
		return true;
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

	inline Element* GetLaneBinHead(int lane) const { return fQueues[lane].Head(); }
	inline bool IsLaneBinEmpty(int lane) const { return fQueues[lane].IsEmpty(); }
	inline Element* GetNextLane(Element* element, int lane) const { return fQueues[lane].GetNext(element); }

	class ConstIterator {
	public:
		ConstIterator() : fQueue(NULL), fLane(kNumLanes - 1), fRT(20), fCurrent(NULL) {}
		ConstIterator(const RunQueue* queue) : fQueue(queue), fLane(kNumLanes - 1), fRT(20), fCurrent(NULL)
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
					fCurrent = fQueue->fQueues[fLane].GetNext(fCurrent);
					if (fCurrent != NULL) return;
					fLane--;
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
			while (fLane >= 0) {
				int word = fLane / (sizeof(native_cpu_mask_t) * 8);
				int bit = fLane % (sizeof(native_cpu_mask_t) * 8);

				native_cpu_mask_t mask = fQueue->GetBitmapWord(word);
				// Filter bits at or below current position in this word
				mask &= ((native_cpu_mask_t)1 << bit) | (((native_cpu_mask_t)1 << bit) - 1);

				if (mask != 0) {
					int nextBit = scheduler_flsnative(mask) - 1;
					fLane = word * (sizeof(native_cpu_mask_t) * 8) + nextBit;
					fCurrent = fQueue->fQueues[fLane].Head();
					if (fCurrent != NULL) return;
				}

				// Skip to next word
				fLane = (word * sizeof(native_cpu_mask_t) * 8) - 1;
			}
			fCurrent = NULL;
		}

		const RunQueue* fQueue;
		int fLane, fRT;
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
	if (IsEmpty()) {
		fSystemVirtualTime = svt;
		return;
	}

	// SVT Advancement: Even if the queue is non-empty, we must advance the
	// local fSystemVirtualTime if it lags significantly behind the core's SVT.
	// This ensures that the deadline quantization buckets (SLI) correctly
	// reflect current scheduler progress.
	if (svt > fSystemVirtualTime + kSVTStagnationThreshold)
		fSystemVirtualTime = svt - kSVTStagnationBuffer;
}

RUN_QUEUE_TEMPLATE_LIST
int32 RUN_QUEUE_CLASS_NAME::_GetLane(bigtime_t deadline, int32 priority) const
{
	int32 fli = (int32)GetSchedulerMatrixRow(priority);

	bigtime_t delta = deadline - fSystemVirtualTime;
	int32 sli = 0;
	if (delta > 0) {
		uint64 reciprocal = LoadAcquire64(gDeadlineBucketReciprocal);
		if (reciprocal > 0) {
			int32 shift = LoadAcquire(gDeadlineBucketShift);
			sli = (int32)(((uint64)delta * reciprocal) >> shift);
		} else {
			int64 bucketSize = LoadAcquire64(gDeadlineBucketSize);
			if (bucketSize <= 0) bucketSize = 5000000;
			sli = (int32)(delta / bucketSize);
		}
		if (sli >= kSecondaryBins) sli = kSecondaryBins - 1;
	}

	// Branchless Inverted Mapping: ensuring higher bit indices always represent
	// higher scheduling priority. This simplifies the hot path selection
	// to a single word-level bit-scan.
	// Band (fli) 15 is better than 0. Within a band, SLI 0 (earliest deadline)
	// is better than SLI 31.
	// Index = (fli * 32) + (31 - sli)
	return (fli << 5) | (31 - sli);
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
		thread->run_queue_lane = -1;
		thread->run_queue_rt_index = index;
	} else {
		int32 lane = _GetLane(thread->virtual_deadline, (int32)priority);
		thread->run_queue_lane = lane;

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
		thread->run_queue_lane = -1;
		thread->run_queue_rt_index = index;
	} else {
		int32 lane = _GetLane(thread->virtual_deadline, (int32)priority);
		thread->run_queue_lane = lane;

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

	if (thread->run_queue_lane < 0) {
		int32 index = thread->run_queue_rt_index;
		fRealTimeQueues[index].Remove(element);
		if (fRealTimeQueues[index].IsEmpty())
			AndAtomic(fRealTimeBitmap, (int32)~(1U << index));
	} else {
		int32 lane = thread->run_queue_lane;
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

	// Branchless O(1) Selection Path for FairShare:
	// Highest set bit in the 512-lane bitmap corresponds to the best thread.
	for (int i = kNumWords - 1; i >= 0; i--) {
		native_cpu_mask_t word = cpu_mask_get_atomic(&fBitmap[i]);
		if (word != 0) {
			int bit = scheduler_flsnative(word) - 1;
			int lane = i * (sizeof(native_cpu_mask_t) * 8) + bit;
			return fQueues[lane].Head();
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

	// FairShare word-level scans
	for (int i = kNumWords - 1; i >= 0; i--) {
		native_cpu_mask_t word = cpu_mask_get_atomic(&fBitmap[i]);
		while (word != 0) {
			int bit = scheduler_flsnative(word) - 1;
			int lane = i * (sizeof(native_cpu_mask_t) * 8) + bit;

			typename DoublyLinkedList<Element, GetLink>::Iterator it = const_cast<DoublyLinkedList<Element, GetLink>&>(fQueues[lane]).GetIterator();
			while (it.HasNext()) {
				Element* element = it.Next();
				if (predicate(element)) {
					if (best == NULL || compare(element, best))
						best = element;
				}
			}
			word &= ~((native_cpu_mask_t)1 << bit);
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
