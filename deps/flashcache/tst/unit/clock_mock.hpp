#ifndef __CLOCK_MOCK_HPP
#define __CLOCK_MOCK_HPP

#include <stdint.h>

uint64_t mockClockGetTimeUs();
void mockClockIncrementTimeInUs(uint64_t delta_us);
void mockClockSetIncrementPerGetTimeCallUs(uint64_t time_us);

#endif  // __CLOCK_MOCK_HPP
