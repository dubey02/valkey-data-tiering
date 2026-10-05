#ifndef __FIO_MOCK_HPP
#define __FIO_MOCK_HPP

#include <string>
#include <errno.h>

// Wrapper functions

extern "C" {
#include "include/fio.h"
#include "include/util.h"

    fioContext *__wrap_fioCreateContext(char const *filename, int mode, size_t queue_depth,
                                        flashcache_monotonic_clock_us monotonic_clock_us);
    ssize_t __wrap_fioAllocate(fioContext *ctx, size_t size);
    void __wrap_fioSubmit(fioContext *ctx, fioRequest *request);
    size_t __wrap_fioGetCompletedRequest(fioContext *ctx, fioRequest ***completed_fio_requests);
    void __wrap_fioReleaseContext(fioContext *ctx);
}

// Functions to change wrapper function behavior

void enableMockFio(size_t min_delay_in_request_processing, bool fifo_ordering, std::string file_with_io_error_);
void disableMockFio();
void mockFioSetLogSize(size_t log_size_bytes);
void mockFioSetMinRequestProcessingDelay(size_t min_request_processing_delay);
size_t mockFioGetMinRequestProcessingDelay();
size_t mockFioGetNumRequestCompleted(fioContext *ctx);
void mockFioPauseRequestProcessing();
void mockFioUnpauseRequestProcessing();
bool mockFioVerifyHeaderFlag(fioContext *ctx, size_t offset, uint32_t expected_flag);
bool mockFioVerifyItem(fioContext *ctx, size_t offset, uint32_t dbid, char const *key, size_t key_len,
        char const *value, size_t value_len, uint32_t flag, flashcache_crc_function crc_function);

#endif  // __FIO_MOCK_HPP
