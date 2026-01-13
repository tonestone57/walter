#ifndef EEVDF_SCHEDULER_H
#define EEVDF_SCHEDULER_H

#include "RBTree.h"
#include "ThreadData.h"
#include "SchedulerErrors.h"

class EevdfScheduler {
public:
    EevdfScheduler()
        :
        _vtime(0)
    {}

    SchedulerError AddThread(ThreadData* thread);
    SchedulerError RemoveThread(ThreadData* thread);
    SchedulerError UpdateThread(ThreadData* thread, int64_t runtime);
    ThreadData* PopMinThread();
    ThreadData* PeekMinThread() const;
    bool IsEmpty() const;
    int Count() const;
    void Clear();

private:
    RBTree _runQueue;
    int64_t _vtime;
    int _count;

    int64_t CalculateVruntime(int64_t delta, int weight);
    void UpdateVtime(ThreadData* minThread);
};

#endif // EEVDF_SCHEDULER_H
