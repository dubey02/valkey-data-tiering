#ifndef __LIBAIO_MOCK_HPP
#define __LIBAIO_MOCK_HPP

// Wrapper functions

extern "C" {
#include "include/fio.h"
#include "include/util.h"

    // LibAio syscall mocks
    int __wrap_io_setup(int maxevents, io_context_t *ctxp);
    int __wrap_io_submit(io_context_t ctx, long nr, struct iocb *ios[]);
    int __wrap_io_getevents(io_context_t ctx_id, long min_nr, long nr, struct io_event *events,
                            struct timespec *timeout);
}

// Functions to change wrapper function behavior

void enableMockLibAio();
void disableMockLibAio();
void setupMockLibAioReturnCodes(int success, int error, int error_times, bool err_in_event);

#endif  // __LIBAIO_MOCK_HPP
