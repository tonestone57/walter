import re
path = 'src/system/kernel/scheduler/RunQueue.h'
with open(path, 'r') as f:
    content = f.read()

# Replace AtomicOr(fRealTimeBitmap, ...) with AtomicOr(&fRealTimeBitmap, ...)?
# No, AtomicOr takes a reference. So AtomicOr(fRealTimeBitmap, ...) is CORRECT.

# Wait, the review said LoadAcquire(bits[i]) was wrong if bits[i] is a value.
# But bits[i] is uint32. LoadAcquire takes a reference.
# Let's check scheduler_common.h wrappers again.

# template <typename T>
# inline int32 LoadAcquire(const T volatile& value) { ... }

# If I call LoadAcquire(fRealTimeBitmap), it passes fRealTimeBitmap by reference. This is CORRECT.

# BUT if I call AtomicAnd64(fBitmap[word], ~((native_cpu_mask_t)1 << bit)),
# fBitmap is an array: native_cpu_mask_t fBitmap[kNumWords];
# fBitmap[word] is an element of that array. Passing it by reference is CORRECT.

# Why did the review say it was wrong?
# "StoreRelease64(&fMeasureTime, ...) is called where the template expects a reference."
# Yes, if I pass a pointer to a reference, it's wrong.
# LoadAcquire64(fMeasureTime) is correct.

# What about Raw atomics?
# "In low_latency.cpp and power_saving.cpp, the raw Haiku function atomic_get64 is called with a value (gIdleNodeSummary) instead of a pointer."
# Yes, atomic_get64 is raw Haiku and expects a pointer.
# But I changed those to LoadAcquire64(gIdleNodeSummary), which is a wrapper that takes a reference.

# Let's check if I have any RAW atomic calls left.
with open(path, 'r') as f:
    content = f.read()

# I see cpu_mask_and_atomic(&fBitmap[word], ~mask)
# cpu_mask_and_atomic is defined in scheduler_common.h:
# static inline native_cpu_mask_t cpu_mask_and_atomic(native_cpu_mask_t volatile* value, ...)
# It takes a POINTER. So &fBitmap[word] is CORRECT.

# Let's double check AtomicAnd64 usage in RunQueue.h line 382.
# AtomicAnd64(fBitmap[word], ~((native_cpu_mask_t)1 << bit));
# AtomicAnd64 takes a reference. fBitmap[word] is the variable. This is CORRECT.

# Maybe the review was complaining about my PREVIOUS version where I was using ::atomic_get64 without &.

# Let's check scheduler.cpp line 2341 (the AtomicOr(target, 0) one)
# I changed it to AtomicOr(*target, 0).
# volatile int32* target = &gCPUEntries[targetCPU].fThreadCount;
# *target is the int32 variable. Passing it by reference to AtomicOr is CORRECT.

# One more thing:
# "Data Corruption (BLOCKING): The handoff logic calls nextThreadData->Dequeue() in reschedule without holding the CPU's runqueue lock."
# In ThreadData::Dequeue():
# inline bool ThreadData::Dequeue() {
#   ...
#   CPURunQueueLocker _(cpu);
#   ...
# }
# It ACQUIRES the lock internally. So calling it in reschedule() is safe?
# Wait, if reschedule() ALREADY holds the lock, then Dequeue() will deadlock.
# reschedule() does NOT hold the cpu runqueue lock yet.
# It holds the oldThread->scheduler_lock.
# It then finds a handoffTarget and acquires handoffTarget->scheduler_lock.
# Then it calls nextThreadData->Dequeue().
# Dequeue() will acquire CPURunQueueLocker(cpu), where cpu is targetCPU.
# Then it calls DonateTimesliceToLocked.

# This seems safe as long as we don't hold the local CPU's runqueue lock.
# In reschedule(), we haven't acquired any CPURunQueueLocker yet.

# Re-check scheduler_thread.h:716
# 	CPURunQueueLocker _(cpu);
# This is indeed inside Dequeue().
