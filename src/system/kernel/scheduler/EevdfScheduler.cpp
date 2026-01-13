#include "EevdfScheduler.h"
#include <support/SupportDefs.h>
#include <algorithm>
#include <cassert>

const int NICE_0_WEIGHT = 1024;

int64_t EevdfScheduler::CalculateVruntime(int64_t delta, int weight) {
    return (delta * NICE_0_WEIGHT) / weight;
}

void EevdfScheduler::UpdateVtime(Scheduler::ThreadData* minThread) {
    if (minThread == nullptr) return;
    _vtime = std::max(_vtime, minThread->VirtualDeadline());
}

status_t EevdfScheduler::AddThread(Scheduler::ThreadData* thread) {
    int64_t vruntime = CalculateVruntime(thread->Runtime(), thread->Weight());
    thread->SetVirtualDeadline(_vtime + vruntime);
    thread->SetLag(0);
    _runQueue.insert(thread);
    _count++;
    return B_OK;
}

status_t EevdfScheduler::RemoveThread(Scheduler::ThreadData* thread) {
    _runQueue.remove(thread);
    _count--;
    return B_OK;
}

status_t EevdfScheduler::UpdateThread(Scheduler::ThreadData* thread, int64_t runtime) {
    _runQueue.remove(thread);
    thread->AddRuntime(runtime);
    thread->SetVirtualDeadline(thread->VirtualDeadline() + CalculateVruntime(runtime, thread->Weight()));
    _runQueue.insert(thread);
    return B_OK;
}

struct thread* EevdfScheduler::PopMinThread() {
    Scheduler::ThreadData* minThreadData = _runQueue.getMinimum();
    if (minThreadData != nullptr) {
        _runQueue.remove(minThreadData);
        _count--;
        UpdateVtime(minThreadData);
        return minThreadData->GetThread();
    }
    return nullptr;
}

struct thread* EevdfScheduler::PeekMinThread() const {
    Scheduler::ThreadData* minThreadData = _runQueue.getMinimum();
    if (minThreadData != nullptr) {
        return minThreadData->GetThread();
    }
    return nullptr;
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
