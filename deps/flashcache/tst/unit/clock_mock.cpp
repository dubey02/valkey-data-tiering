#include "clock_mock.hpp"

static uint64_t current_time_us = 0;

// Each clock_gettime call increment the time by the specified delta
static uint64_t time_increment_per_gettime_call_us = 100;

uint64_t mockClockGetTimeUs() {
    uint64_t res = current_time_us;
    mockClockIncrementTimeInUs(time_increment_per_gettime_call_us);
    return res;
}

void mockClockIncrementTimeInUs(uint64_t delta_us) {
    current_time_us += delta_us;
}

void mockClockSetIncrementPerGetTimeCallUs(uint64_t time_us) {
    time_increment_per_gettime_call_us = time_us;
}
