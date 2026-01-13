#include "EevdfScheduler.h"
#include <support/SupportDefs.h>
#include <algorithm>
#include <cassert>

const int NICE_0_WEIGHT = 1024;

int64_t EevdfScheduler::CalculateVruntime(int64_t delta, int weight) {
    return (delta * NICE_0_WEIGHT) / weight;
}

void EevdfScheduler::UpdateVtime(ThreadData* minThread) {
    if (minThread == nullptr) return;
    _vtime = std::max(_vtime, minThread->VirtualDeadline());
}

SchedulerError EevdfScheduler::AddThread(ThreadData* thread) {
    assert(thread != nullptr);
    int64_t vruntime = CalculateVruntime(thread->Runtime(), thread->Weight());
    _vtime = std::max(_vtime, vruntime);
    thread->SetVirtualDeadline(_vtime + vruntime);
    thread->SetLag(0);
    _runQueue.insert(thread);
    _count++;
    return SCHED_OK;
}

SchedulerError EevdfScheduler::RemoveThread(ThreadData* thread) {
    assert(thread != nullptr);
    _runQueue.remove(thread);
    _count--;
    return SCHED_OK;
}

SchedulerError EevdfScheduler::UpdateThread(ThreadData* thread, int64_t runtime) {
    assert(thread != nullptr);
    _runQueue.remove(thread);
    thread->AddRuntime(runtime);
    thread->SetVirtualDeadline(thread->VirtualDeadline() + CalculateVruntime(runtime, thread->Weight()));
    _runQueue.insert(thread);
    return SCHED_OK;
}

ThreadData* EevdfScheduler::PopMinThread() {
    ThreadData* minThread = _runQueue.getMinimum();
    if (minThread != nullptr) {
        _runQueue.remove(minThread);
        _count--;
        UpdateVtime(minThread);
    }
    return minThread;
}

ThreadData* EevdfScheduler::PeekMinThread() const {
    return _runQueue.getMinimum();
}

bool EevdfScheduler::IsEmpty() const {
    return _count == 0;
}

int EevdfScheduler::Count() const {
    return _count;
}

void EevdfScheduler::Clear() {
    _runQueue.clear();
    _count = 0;
}
