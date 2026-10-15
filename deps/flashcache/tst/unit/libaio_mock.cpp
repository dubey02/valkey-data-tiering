#include <stdlib.h>
#include <stdint.h>

#include "libaio_mock.hpp"

extern "C" {
    int __real_io_setup(int maxevents, io_context_t *ctxp);
    int __real_io_submit(io_context_t ctx, long nr, struct iocb *ios[]);
    int __real_io_getevents(io_context_t ctx_id, long min_nr, long nr, struct io_event *events,
                            struct timespec *timeout);
}

static bool is_mock_enabled = false;
static int success_return_code = 0;
static int error_return_code = 0;
static int total_error_times = 0;
static int num_error_returns = 0;
static bool error_returned_in_event = false;

void enableMockLibAio() {
    is_mock_enabled = true;
}

void disableMockLibAio() {
    is_mock_enabled = false;
    // Clear the return codes
    success_return_code = 0;
    error_return_code = 0;
    total_error_times = 0;
    num_error_returns = 0;
}

void setupMockLibAioReturnCodes(int success, int error, int error_times, bool err_in_event) {
    success_return_code = success;
    error_return_code = error;
    total_error_times = error_times;
    num_error_returns = 0;
    error_returned_in_event = err_in_event;
}

static int nextSuccessOrErrorCodeForMock() {
    if (error_return_code != 0 && num_error_returns < total_error_times) {
        num_error_returns++;
        return error_return_code;
    } else {
        return success_return_code;
    }
}

int __wrap_io_setup(int maxevents, io_context_t *ctxp) {
    if (is_mock_enabled) {
        return nextSuccessOrErrorCodeForMock();
    }

    return __real_io_setup(maxevents, ctxp);
}

int __wrap_io_submit(io_context_t ctx, long nr, struct iocb *ios[]) {
    if (is_mock_enabled) {
        return nextSuccessOrErrorCodeForMock();
    }

    return __real_io_submit(ctx, nr, ios);
}

int __wrap_io_getevents(io_context_t ctx_id, long min_nr, long nr,
                        struct io_event *events, struct timespec *timeout) {
    if (is_mock_enabled) {
        if (num_error_returns < total_error_times) {
            num_error_returns++;
            if (!error_returned_in_event) {
                return error_return_code;
            } else {
                // populate the event with error code
                events[0].res = error_return_code;
                return success_return_code;
            }
        } else {
            events[0].res = success_return_code;
            return success_return_code;
        }
    }

    return __real_io_getevents(ctx_id, min_nr, nr, events, timeout);
}
