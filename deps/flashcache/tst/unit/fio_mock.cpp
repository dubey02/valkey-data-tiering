#include <vector>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <map>
#include <cstdlib>
#include <algorithm>

#include "fio_mock.hpp"

extern "C" {
#include "include/util.h"
#include "include/serialization.h"

    fioContext *__real_fioCreateContext(char const *filename, int mode, size_t queue_depth,
                                        flashcache_monotonic_clock_us monotonic_clock_us);
    ssize_t __real_fioAllocate(fioContext *ctx, size_t size);
    void __real_fioSubmit(fioContext *ctx, fioRequest *request);
    size_t __real_fioGetCompletedRequest(fioContext *ctx, fioRequest ***completed_fio_requests);
    void __real_fioReleaseContext(fioContext *ctx);
}

typedef struct {
    char *data;
    size_t size_bytes;
} inMemoryStore;

typedef struct {
    size_t receive_time;
    fioRequest *request;
} completedRequest;

typedef struct {
    inMemoryStore *store;
    size_t num_request_completed;
    std::vector<completedRequest*> completed_requests;
} inMemoryIoContext;

std::map<std::string, inMemoryStore*> filename_to_store_map;
std::map<fioContext*, inMemoryIoContext*> io_context_map;

static bool is_mock_enabled = false;
static bool is_processing_paused = false;
static size_t size_bytes = 0;
std::string file_with_io_error = "";

// Logical time is updated by 1 when a request is submitted using fioSubmit or getCompleteRequest
// API is called.
static size_t logical_time = 0;

// The minimum delay in terms of logical time between the time when the request is submitted to the
// time when the response is sent back.
static size_t min_request_processing_delay = 0;

// When FIFO ordering is enabled, the responses for the requests are sent back in the same order
// in which they were received.
static bool fifo_ordering_enabled = false;

static bool isRequestForFileWithIOError(inMemoryStore *store) {
    auto it = filename_to_store_map.find(file_with_io_error);
    if (it == filename_to_store_map.end()) {
       return false;
    }
    if (it->second == store) {
        return true;
    }
    return false;
}

void enableMockFio(size_t min_delay_in_request_processing, bool fifo_ordering, std::string file_with_io_error_) {
    is_mock_enabled = true;
    size_bytes = 100 * 1024 * 1024;  // 100 MiB
    min_request_processing_delay = min_delay_in_request_processing;
    fifo_ordering_enabled = fifo_ordering;
    std::srand(42);
    is_processing_paused = false;
    file_with_io_error = file_with_io_error_;
}

void mockFioSetLogSize(size_t log_size_bytes) {
    size_bytes = log_size_bytes;
}

void mockFioSetMinRequestProcessingDelay(size_t request_processing_delay) {
    min_request_processing_delay = request_processing_delay;
}

size_t mockFioGetMinRequestProcessingDelay() {
    return min_request_processing_delay;
}

void disableMockFio() {
    is_mock_enabled = false;
    for (auto it = filename_to_store_map.begin(); it != filename_to_store_map.end(); ++it) {
        fcFree(it->second->data);
        delete it->second;
    }
    filename_to_store_map.clear();

    for (auto it = io_context_map.begin(); it != io_context_map.end(); ++it) {
        delete it->first;
        delete it->second;
    }
    io_context_map.clear();
}

size_t mockFioGetNumRequestCompleted(fioContext *ctx) {
    inMemoryIoContext *mem_io_ctx = io_context_map.find(ctx)->second;
    return mem_io_ctx->num_request_completed;
}

void mockFioPauseRequestProcessing() {
    is_processing_paused = true;
}

void mockFioUnpauseRequestProcessing() {
    is_processing_paused = false;
}

bool mockFioVerifyHeaderFlag(fioContext *ctx, size_t offset, uint32_t expected_flag) {
    inMemoryIoContext *mem_io_ctx = io_context_map.find(ctx)->second;
    inMemoryStore *store = mem_io_ctx->store;
    return (getFlagInSerializedItem(store->data + offset) == expected_flag);
}

bool mockFioVerifyItem(fioContext *ctx, size_t offset, uint32_t dbid, char const *key, size_t key_len,
        char const *value, size_t value_len, uint32_t flag, flashcache_crc_function crc_function) {
    inMemoryIoContext *mem_io_ctx = io_context_map.find(ctx)->second;
    inMemoryStore *store = mem_io_ctx->store;
    char *serialized_item = nullptr;
    size_t serialized_item_len = 0;

    serializeKeyValuePair(dbid, key, key_len, value, value_len, &serialized_item, &serialized_item_len,
            crc_function);
    updateFlagInSerializedItem(serialized_item, flag, crc_function);

    if (offset + serialized_item_len > store->size_bytes) {
        return false;
    }

    bool res = (!memcmp(store->data + offset, serialized_item, serialized_item_len));
    fcFree(serialized_item);
    return res;
}

fioContext *__wrap_fioCreateContext(char const *filename, int mode, size_t queue_depth,
                                    flashcache_monotonic_clock_us monotonic_clock_us) {
    if (is_mock_enabled) {
        std::string filename_str(filename);
        if (filename_str == file_with_io_error) {
            return NULL;
        }
        if (mode != FILE_READ_ONLY && mode != FILE_READ_WRITE) {
            return NULL;
        }
        auto it = filename_to_store_map.find(filename_str);
        inMemoryStore *store = nullptr;
        if (it == filename_to_store_map.end()) {
            store = new inMemoryStore;
            store->data = reinterpret_cast<char *>(fcMalloc(sizeof(char) * size_bytes));
            store->size_bytes = size_bytes;
            filename_to_store_map[filename_str] = store;
        } else {
            store = it->second;
        }

        fioContext *ctx = new fioContext;
        ctx->queue_depth = queue_depth;
        ctx->num_inflight_request = 0;
        ctx->completed_fio_requests = reinterpret_cast<fioRequest **>(fcMalloc(sizeof(fioRequest *) * queue_depth));

        inMemoryIoContext *mem_io_ctx = new inMemoryIoContext;
        mem_io_ctx->num_request_completed = 0;
        mem_io_ctx->store = store;
        io_context_map[ctx] = mem_io_ctx;
        return ctx;
    }

    return __real_fioCreateContext(filename, mode, queue_depth, monotonic_clock_us);
}

ssize_t __wrap_fioAllocate(fioContext *ctx, size_t size) {
    if (is_mock_enabled) {
        inMemoryIoContext *mem_io_ctx = io_context_map.find(ctx)->second;
        inMemoryStore *store = mem_io_ctx->store;
        if (isRequestForFileWithIOError(store)) {
            return 1;
        }
        store->data = static_cast<char *>(fcRealloc(store->data, size));
        store->size_bytes = size;
        return 0;
    }

    return __real_fioAllocate(ctx, size);
}

void __wrap_fioSubmit(fioContext *ctx, fioRequest *request) {
    if (is_mock_enabled) {
        flashcacheAssert(ctx->num_inflight_request < ctx->queue_depth);

        char *buf = request->data_buffer;
        size_t buf_size = request->data_buffer_size;
        size_t offset = request->offset;
        inMemoryIoContext *mem_io_ctx = io_context_map.find(ctx)->second;
        inMemoryStore *store = mem_io_ctx->store;

        if (request->op == FC_FIO_WRITE) {
            if (offset + buf_size > store->size_bytes) {
                store->size_bytes = offset + buf_size;
                store->data = static_cast<char *>(fcRealloc(store->data,
                            store->size_bytes));
            }
            memcpy(store->data + offset, buf, buf_size);
        } else if (request->op == FC_FIO_READ) {
            flashcacheAssert(offset < store->size_bytes);
            flashcacheAssert(buf_size <= store->size_bytes);
            flashcacheAssert(offset + buf_size <= store->size_bytes);
            memcpy(buf, store->data + offset, buf_size);
        } else {
            flashcacheAssert(false);
        }

        ctx->num_inflight_request++;
        completedRequest *completed_request = new completedRequest;
        completed_request->request = request;
        completed_request->receive_time = logical_time;
        mem_io_ctx->completed_requests.push_back(completed_request);
        logical_time++;
        return;
    }

    __real_fioSubmit(ctx, request);
}

size_t __wrap_fioGetCompletedRequest(fioContext *ctx, fioRequest ***completed_fio_requests) {
    if (is_mock_enabled) {
        if (is_processing_paused) {
            return 0;
        }

        inMemoryIoContext *mem_io_ctx = io_context_map.find(ctx)->second;
        inMemoryStore *store = mem_io_ctx->store;
        int num_request_completed = 0;
        if (!fifo_ordering_enabled) {
            std::random_shuffle(mem_io_ctx->completed_requests.begin(), mem_io_ctx->completed_requests.end());
        }
        for (auto it = mem_io_ctx->completed_requests.begin(); it != mem_io_ctx->completed_requests.end();) {
            auto completed_request = (*it);
            if (logical_time - completed_request->receive_time >= min_request_processing_delay) {
                ctx->completed_fio_requests[num_request_completed++] = completed_request->request;
                if (isRequestForFileWithIOError(store)) {
                    completed_request->request->err_no = -EINVAL;
                }
                it = mem_io_ctx->completed_requests.erase(it);
                delete completed_request;
            } else {
                ++it;
            }
        }
        (*completed_fio_requests) = ctx->completed_fio_requests;
        ctx->num_inflight_request -= num_request_completed;
        mem_io_ctx->num_request_completed += num_request_completed;
        logical_time++;
        return num_request_completed;
    }
    return __real_fioGetCompletedRequest(ctx, completed_fio_requests);
}

void __wrap_fioReleaseContext(fioContext *ctx) {
    if (is_mock_enabled) {
        auto it = io_context_map.find(ctx);
        inMemoryIoContext *mem_io_ctx = it->second;
        io_context_map.erase(it);

        for (size_t i = 0; i < mem_io_ctx->completed_requests.size(); ++i) {
            completedRequest *completed_request = mem_io_ctx->completed_requests[i];
            fioRequest *fio_request = completed_request->request;
            fcFree(fioRequestGetBuffer(fio_request));
            fioRequestClear(fio_request);
            delete completed_request;
        }

        fcFree(ctx->completed_fio_requests);
        delete mem_io_ctx;
        delete ctx;
        return;
    }

    __real_fioReleaseContext(ctx);
}
