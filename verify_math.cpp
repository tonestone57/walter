#include <iostream>
#include <algorithm>

typedef long long bigtime_t;
typedef int int32;
typedef long long int64;

#define min_c(a,b) std::min(a,b)
#define max_c(a,b) std::max(a,b)

int main() {
    int32 fInteractivityScore = 500;
    bigtime_t quantum = 3000;

    // Test quantum scaling
    bigtime_t scaled_quantum = quantum * (1500 - fInteractivityScore) / 1000;
    std::cout << "Neutral Score 500: " << scaled_quantum << " (Expected 3000)" << std::endl;

    fInteractivityScore = 1000;
    scaled_quantum = quantum * (1500 - fInteractivityScore) / 1000;
    std::cout << "Bursty Score 1000: " << scaled_quantum << " (Expected 1500)" << std::endl;

    fInteractivityScore = 0;
    scaled_quantum = quantum * (1500 - fInteractivityScore) / 1000;
    std::cout << "Batch Score 0: " << scaled_quantum << " (Expected 4500)" << std::endl;

    // Test deadline scaling
    bigtime_t slice = 5000;
    fInteractivityScore = 1000;
    bigtime_t scaled_slice = slice * (1500 - fInteractivityScore) / 1000;
    std::cout << "Bursty Slice: " << scaled_slice << " (Expected 2500)" << std::endl;

    // Test urgency boost
    int64 gDeadlineBucketSize = 5000;
    fInteractivityScore = 1000;
    bigtime_t urgencyBoost = (fInteractivityScore * gDeadlineBucketSize) / 1000;
    std::cout << "Bursty Urgency Boost: " << urgencyBoost << " (Expected 5000)" << std::endl;

    return 0;
}
