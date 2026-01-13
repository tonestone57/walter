#ifndef EEVDF_THREAD_DATA_H
#define EEVDF_THREAD_DATA_H

#include <stdint.h>

class ThreadData {
public:
    ThreadData(int id, int64_t deadline)
        : _id(id),
          _virtualDeadline(deadline),
          _runtime(0)
    {}

    int ID() const { return _id; }

    // The deadline used by EEVDF scheduler
    int64_t VirtualDeadline() const { return _virtualDeadline; }

    void SetVirtualDeadline(int64_t deadline) {
        _virtualDeadline = deadline;
    }

    int64_t Runtime() const { return _runtime; }

    void AddRuntime(int64_t delta) {
        _runtime += delta;
    }

private:
    int _id;                    // Unique thread identifier
    int64_t _virtualDeadline;   // For EEVDF prioritization
    int64_t _runtime;           // Accumulated run time
    int64_t _lag;               // For EEVDF lag
    int _weight;                // For EEVDF weight

public:
    int64_t Lag() const { return _lag; }
    void SetLag(int64_t lag) { _lag = lag; }

    int Weight() const { return _weight; }
    void SetWeight(int weight) { _weight = weight; }
};

#endif // EEVDF_THREAD_DATA_H
