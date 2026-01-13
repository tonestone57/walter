#ifndef EEVDF_SCHEDULER_H
#define EEVDF_SCHEDULER_H

#include "scheduler_thread.h"
#include "RBTree.h"

class EevdfScheduler {
public:
    EevdfScheduler()
        :
        _vtime(0),
        _count(0)
    {}

    status_t AddThread(Scheduler::ThreadData* thread);
    status_t RemoveThread(Scheduler::ThreadData* thread);
    status_t UpdateThread(Scheduler::ThreadData* thread, int64_t runtime);
    struct thread* PopMinThread();
    struct thread* PeekMinThread() const;
    bool IsEmpty() const;
    int Count() const;
    void Clear();

private:
    RBTree _runQueue;
    int64_t _vtime;
    int _count;

    int64_t CalculateVruntime(int64_t delta, int weight);
    void UpdateVtime(Scheduler::ThreadData* minThread);
};

#endif // EEVDF_SCHEDULER_H
