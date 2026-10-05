#include <stdlib.h>
#include <gtest/gtest.h>
#include <set>

#include "flashcache_test_base.hpp"
#include "clock_mock.hpp"
#include "libaio_mock.hpp"

extern "C" {
#include <sys/stat.h>
#include "include/fio.h"
#include "include/util.h"

    fioContext *__real_fioCreateContext(char const *filename, int mode, size_t queue_depth,
                                        flashcache_monotonic_clock_us monotonic_clock_us);
    void __real_fioSubmit(fioContext *ctx, fioRequest *request);
    size_t __real_fioGetCompletedRequest(fioContext *ctx, fioRequest ***completed_fio_requests);
    void __real_fioReleaseContext(fioContext *ctx);
}

#define QUEUE_DEPTH (32)

class FioTest : public flashcache::FlashcacheTestBase, public testing::Test {
    FILE *file = NULL;
    char filename[FILENAME_MAX] = { 0 };

    void SetUp() {
        file = std::tmpfile();
        extractFilename(fileno(file), filename);
        ctx = __real_fioCreateContext(filename, FILE_READ_WRITE, QUEUE_DEPTH, mockClockGetTimeUs);
    }

    void TearDown() {
        disableMockLibAio();
        if (ctx != nullptr) {
            __real_fioReleaseContext(ctx);
        }
        fclose(file);
        unlink(filename);
    }

 public:
     fioContext *ctx;
     std::set<std::tuple<size_t, size_t>> used_file_block;

     void initializeFileWithZero(size_t filesize) {
         char *filedata = reinterpret_cast<char*>(fcPosixMemalign(FC_PAGESIZE,
                     filesize));
         memset(filedata, 0, filesize);

         fioRequest *request = new fioRequest;
         fioRequest **completed_requests = nullptr;
         fioRequestFill(request, FC_FIO_WRITE, filedata, filesize, 0, FC_FIO_WRITE);
         __real_fioSubmit(ctx, request);
         while (__real_fioGetCompletedRequest(ctx, &completed_requests) == 0) {
             // Busy spin
         }
         delete completed_requests[0];
         fcFree(filedata);
     }

     std::tuple<size_t, size_t> getFileBlockForIO(size_t filesize) {
         size_t num_pages = (filesize / FC_PAGESIZE);
         size_t start_offset = 0;
         size_t blocksize = 0;
         size_t end_offset = 0;
         while (true) {
             start_offset = (random() % num_pages) * FC_PAGESIZE;
             blocksize = ((random() % 25) + 1) * FC_PAGESIZE;  // Maximum blocksize can be 100 KiB
             end_offset = start_offset + blocksize - 1;
             if (end_offset >= filesize) {
                 blocksize = filesize - start_offset;
                 end_offset = filesize - 1;
             }

             bool unused_block = true;
             for (auto it = used_file_block.begin(); it != used_file_block.end(); ++it) {
                 size_t used_block_start_offset = std::get<0>(*it);
                 size_t used_block_end_offset = used_block_start_offset + std::get<1>(*it) - 1;
                 if (((start_offset >= used_block_start_offset) && (start_offset <= used_block_end_offset)) ||
                         ((end_offset >= used_block_start_offset) && (end_offset <= used_block_end_offset)) ||
                         ((used_block_start_offset >= start_offset) && (used_block_start_offset <= end_offset)) ||
                         ((used_block_end_offset >= start_offset) && (used_block_end_offset <= end_offset))) {
                     unused_block = false;
                     break;
                 }
             }

             if (unused_block) {
                 break;
             }
         }

         return std::make_tuple(start_offset, blocksize);
     }

     size_t getFileSize() {
         struct stat st;
         stat(filename, &st);
         return st.st_size;
     }

    int getHistogramTotalCount(unsigned long long histogram[], const size_t histogram_size) {
        int total_count = 0;
        for (int i = 0; i < histogram_size; i++) {
            total_count += histogram[i];
        }
        return total_count;
    }
};

TEST_F(FioTest, testAllocate) {
    ASSERT_EQ(getFileSize(), 0);

    size_t sz = 8192;
    fioAllocate(ctx, sz);
    ASSERT_EQ(getFileSize(), sz);

    sz = 4096;
    fioAllocate(ctx, sz);
    ASSERT_EQ(getFileSize(), sz);
}

TEST_F(FioTest, testWriteAndRead) {
    fioRequest **completed_requests = nullptr;
    size_t write_user_data = 3212L;
    size_t write_buf_size = 8192;
    char *write_buf = nullptr;

    const size_t histogram_size = 9;
    unsigned long long read_histogram[histogram_size] = {0};
    unsigned long long write_histogram[histogram_size] = {0};


    write_buf = reinterpret_cast<char*>(fcPosixMemalign(FC_PAGESIZE, write_buf_size));
    fillRandomData(write_buf, write_buf_size);
    fioRequest request = { 0 };
    fioRequestFill(&request, write_user_data, write_buf, write_buf_size, 0, FC_FIO_WRITE);
    __real_fioSubmit(ctx, &request);
    while (__real_fioGetCompletedRequest(ctx, &completed_requests) != 1) {
        // busy spin
    }

    ASSERT_EQ(completed_requests[0], &request);
    ASSERT_EQ(completed_requests[0]->user_data, write_user_data);

    // Write Histogram assertion
    fioGetHistogramMetrics(FC_DISK_WRITE_LATENCY_HISTOGRAM, write_histogram, histogram_size);
    ASSERT_EQ(1, getHistogramTotalCount(write_histogram, histogram_size));

    // Read Histogram assertion
    fioGetHistogramMetrics(FC_DISK_READ_LATENCY_HISTOGRAM, read_histogram, histogram_size);
    ASSERT_EQ(0, getHistogramTotalCount(read_histogram, histogram_size));

    size_t read_user_data = 765431L;
    char *read_buf = nullptr;
    read_buf = reinterpret_cast<char*>(fcPosixMemalign(FC_PAGESIZE, write_buf_size));
    for (size_t read_buf_size = FC_PAGESIZE; read_buf_size <= write_buf_size; read_buf_size += FC_PAGESIZE) {
        fioRequestFill(&request, read_user_data, read_buf, read_buf_size, 0, FC_FIO_READ);
        __real_fioSubmit(ctx, &request);
        while (__real_fioGetCompletedRequest(ctx, &completed_requests) != 1) {
            // busy spin
        }
        ASSERT_EQ(completed_requests[0], &request);
        ASSERT_EQ(completed_requests[0]->user_data, read_user_data);
        ASSERT_EQ(0, memcmp(fioRequestGetBuffer(completed_requests[0]), write_buf, read_buf_size));
        ASSERT_EQ(fioRequestGetBufferSize(completed_requests[0]), read_buf_size);

        // Read Histogram assertion
        fioGetHistogramMetrics(FC_DISK_READ_LATENCY_HISTOGRAM, read_histogram, histogram_size);
        ASSERT_EQ(getHistogramTotalCount(read_histogram, histogram_size), static_cast<int>(read_buf_size/FC_PAGESIZE));
        read_user_data++;
    }

    fcFree(read_buf);
    fcFree(write_buf);
}

TEST_F(FioTest, testParallelRequest) {
    size_t filesize = 10L * 1024 * 1024;  // 10 MiB

    char *filedata = new char[filesize];
    memset(filedata, 0, filesize);
    initializeFileWithZero(filesize);

    size_t num_request_completed = 0;
    size_t num_request_initiated = 0;
    size_t total_num_request = 300;
    fioRequest **completed_requests = nullptr;
    while (num_request_completed < total_num_request) {
        while ((num_request_initiated < total_num_request) && (used_file_block.size() < QUEUE_DEPTH)) {
            std::tuple<size_t, size_t> io_block = getFileBlockForIO(filesize);
            used_file_block.insert(io_block);

            size_t offset = std::get<0>(io_block);
            size_t blocksize = std::get<1>(io_block);
            char *buf = nullptr;
            buf = reinterpret_cast<char*>(fcPosixMemalign(FC_PAGESIZE, blocksize));
            fioRequest *request = new fioRequest;

            int op = num_request_initiated % 2;
            if (op == 1) {
                // Perform write
                fillRandomData(buf, blocksize);
                fioRequestFill(request, op, buf, blocksize, offset, FC_FIO_WRITE);
            } else {
                // Perform read
                fioRequestFill(request, op, buf, blocksize, offset, FC_FIO_READ);
            }
            __real_fioSubmit(ctx, request);
            num_request_initiated++;
        }

        size_t num_events = __real_fioGetCompletedRequest(ctx, &completed_requests);
        for (size_t i = 0; i < num_events; ++i) {
            fioRequest *request = completed_requests[i];
            size_t offset = request->offset;
            char *buf = fioRequestGetBuffer(request);
            size_t buf_size = fioRequestGetBufferSize(request);
            if (request->user_data == 0) {
                ASSERT_EQ(0, memcmp((filedata + offset), buf, buf_size));
            } else if (request->user_data == 1) {
                memcpy((filedata + offset), buf, buf_size);
            } else {
                ASSERT_TRUE(false);
            }
            used_file_block.erase(used_file_block.find(std::make_tuple(offset, buf_size)));
            fcFree(buf);
            num_request_completed++;
            delete request;
        }
    }

    delete[] filedata;
}

TEST_F(FioTest, testReleaseContextWithInflightRequest) {
    size_t write_user_data = 3212L;
    size_t write_buf_size = 8192;
    char *write_buf = nullptr;
    write_buf = reinterpret_cast<char*>(fcPosixMemalign(FC_PAGESIZE, write_buf_size));
    fillRandomData(write_buf, write_buf_size);
    fioRequest request = { 0 };
    fioRequestFill(&request, write_user_data, write_buf, write_buf_size, 0, FC_FIO_WRITE);
    __real_fioSubmit(ctx, &request);
    __real_fioReleaseContext(ctx);
    ctx = nullptr;
}

TEST_F(FioTest, testLibAioSetupFailure) {
    enableMockLibAio();
    // Retry and succeed
    setupMockLibAioReturnCodes(0, -EAGAIN, 1, false);
    fioContext *context = fioCreateContext(ctx->filename, FILE_READ_WRITE, QUEUE_DEPTH, mockClockGetTimeUs);
    ASSERT_NE(context, nullptr);
    fioReleaseContext(context);
    ASSERT_EQ(fioGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR), 1);

    // Retry threshold exceeded and fail
    setupMockLibAioReturnCodes(0, -EAGAIN, FC_MAX_IO_OPERATION_RETRY+1, false);
    ASSERT_DEATH(fioCreateContext(ctx->filename, FILE_READ_WRITE, QUEUE_DEPTH, mockClockGetTimeUs), "");

    // An invalid pointer is passed for context
    setupMockLibAioReturnCodes(0, -EFAULT, 1, false);
    ASSERT_DEATH(fioCreateContext(ctx->filename, FILE_READ_WRITE, QUEUE_DEPTH, mockClockGetTimeUs), "");

    // Context is not initialized, or specified nr_events exceeds internal limits
    setupMockLibAioReturnCodes(0, -EINVAL, 1, false);
    ASSERT_DEATH(fioCreateContext(ctx->filename, FILE_READ_WRITE, QUEUE_DEPTH, mockClockGetTimeUs), "");

    // Insufficient kernel resources are available
    setupMockLibAioReturnCodes(0, -ENOMEM, 1, false);
    ASSERT_DEATH(fioCreateContext(ctx->filename, FILE_READ_WRITE, QUEUE_DEPTH, mockClockGetTimeUs), "");

    // Not supported on this architecture
    setupMockLibAioReturnCodes(0, -ENOSYS, 1, false);
    ASSERT_DEATH(fioCreateContext(ctx->filename, FILE_READ_WRITE, QUEUE_DEPTH, mockClockGetTimeUs), "");
}

TEST_F(FioTest, testLibAioIoSubmitFailure) {
    enableMockLibAio();
    size_t write_user_data = 3212L;
    size_t write_buf_size = 8192;
    char *write_buf = nullptr;
    write_buf = reinterpret_cast<char*>(fcPosixMemalign(FC_PAGESIZE, write_buf_size));
    fillRandomData(write_buf, write_buf_size);
    fioRequest request = { 0 };
    fioRequestFill(&request, write_user_data, write_buf, write_buf_size, 0, FC_FIO_WRITE);

    // Retry on insufficient resources available to queue any iocbs
    int num_retry = 10;
    setupMockLibAioReturnCodes(1, -EAGAIN, num_retry, false);
    fioSubmit(ctx, &request);
    ASSERT_EQ(ctx->num_inflight_request, 1);
    ASSERT_EQ(fioGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR), num_retry);

    // Retry limit exceeded
    ctx->num_inflight_request = 0;
    setupMockLibAioReturnCodes(1, -EAGAIN, FC_MAX_IO_OPERATION_RETRY+1, false);
    ASSERT_DEATH(fioSubmit(ctx, &request), "");

    // An invalid pointer is passed for one of the data structures
    setupMockLibAioReturnCodes(1, -EFAULT, 1, false);
    ASSERT_DEATH(fioSubmit(ctx, &request), "");

    // AIO context is not valid
    setupMockLibAioReturnCodes(1, -EINVAL, 1, false);
    ASSERT_DEATH(fioSubmit(ctx, &request), "");

    // Bad file descriptor specified
    setupMockLibAioReturnCodes(1, -EBADF, 1, false);
    ASSERT_DEATH(fioSubmit(ctx, &request), "");

    // Not supported on this architecture
    setupMockLibAioReturnCodes(1, -ENOSYS, 1, false);
    ASSERT_DEATH(fioSubmit(ctx, &request), "");

    // aio_reqprio field is set with IOPRIO_CLASS_RT, but the submitting
    // context doesn't have CAP_SYS_ADMIN capability.
    setupMockLibAioReturnCodes(1, -EPERM, 1, false);
    ASSERT_DEATH(fioSubmit(ctx, &request), "");

    fcFree(write_buf);
}

TEST_F(FioTest, testLibAioIoGetEventsFailure) {
    enableMockLibAio();
    enableMockFio(10, true, "");
    ctx->num_inflight_request = 1;

    // Retryable errors
    int num_retry = 1;
    setupMockLibAioReturnCodes(0, -EAGAIN, 1, false);
    ASSERT_EQ(__real_fioGetCompletedRequest(ctx, nullptr), 0);
    ASSERT_EQ(fioGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR), num_retry++);

    setupMockLibAioReturnCodes(0, -EINTR, 1, false);
    ASSERT_EQ(__real_fioGetCompletedRequest(ctx, nullptr), 0);
    ASSERT_EQ(fioGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR), num_retry++);

    // An invalid pointer is passed for events or timeout
    setupMockLibAioReturnCodes(0, -EFAULT, 1, false);
    ASSERT_DEATH(__real_fioGetCompletedRequest(ctx, nullptr), "");

    // AIO context is not valid, min_nr or nr is out of range
    setupMockLibAioReturnCodes(0, -EINVAL, 1, false);
    ASSERT_DEATH(__real_fioGetCompletedRequest(ctx, nullptr), "");

    // Not supported on this architecture
    setupMockLibAioReturnCodes(0, -ENOSYS, 1, false);
    ASSERT_DEATH(__real_fioGetCompletedRequest(ctx, nullptr), "");

    // Test error return code inside the returned event
    fioRequest **completed_requests = nullptr;
    ctx->num_inflight_request = 1;

    // Populate context with events data
    size_t data_size = 8192;
    char *buf = reinterpret_cast<char*>(fcPosixMemalign(FC_PAGESIZE, data_size));
    fioRequest request = { 0 };
    memset(buf, 0, data_size);
    fioRequestFill(&request, 1, buf, data_size, 0, FC_FIO_WRITE);
    ctx->io_events[0].data = &request;
    ctx->io_events[0].res2 = 0;
    struct iocb control_buffer = {};
    ctx->io_events[0].obj = &control_buffer;
    ctx->io_events[0].obj->u.c.nbytes = data_size;
    ctx->io_events[0].obj->u.c.offset = 0;

    // Retryable errors
    setupMockLibAioReturnCodes(1, -EAGAIN, 1, true);
    ASSERT_EQ(__real_fioGetCompletedRequest(ctx, &completed_requests), 0);
    ASSERT_EQ(ctx->num_inflight_request, 1);
    ASSERT_EQ(fioGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR), num_retry++);

    setupMockLibAioReturnCodes(1, -EINTR, 1, true);
    ASSERT_EQ(__real_fioGetCompletedRequest(ctx, &completed_requests), 0);
    ASSERT_EQ(ctx->num_inflight_request, 1);
    ASSERT_EQ(fioGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR), num_retry++);

    setupMockLibAioReturnCodes(1, 0, 1, true);
    ASSERT_EQ(__real_fioGetCompletedRequest(ctx, &completed_requests), 0);
    ASSERT_EQ(ctx->num_inflight_request, 1);
    ASSERT_EQ(fioGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR), num_retry++);

    setupMockLibAioReturnCodes(1, data_size - 1, 1, true);
    ASSERT_EQ(__real_fioGetCompletedRequest(ctx, &completed_requests), 0);
    ASSERT_EQ(ctx->num_inflight_request, 1);
    ASSERT_EQ(fioGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR), num_retry++);

    // Transferred bytes is larger than the data provided
    setupMockLibAioReturnCodes(1, data_size + 1, 1, true);
    ASSERT_DEATH(__real_fioGetCompletedRequest(ctx, &completed_requests), "");

    // An invalid pointer is passed for events or timeout
    setupMockLibAioReturnCodes(1, -EFAULT, 1, true);
    ctx->num_inflight_request = 1;
    ASSERT_EQ(__real_fioGetCompletedRequest(ctx, &completed_requests), 1);
    ASSERT_EQ(completed_requests[0]->err_no, -EFAULT);

    // AIO context is not valid, min_nr or nr is out of range
    setupMockLibAioReturnCodes(1, -EINVAL, 1, true);
    ctx->num_inflight_request = 1;
    ASSERT_EQ(__real_fioGetCompletedRequest(ctx, &completed_requests), 1);
    ASSERT_EQ(completed_requests[0]->err_no, -EINVAL);

    // Not supported on this architecture
    setupMockLibAioReturnCodes(1, -ENOSYS, 1, true);
    ctx->num_inflight_request = 1;
    ASSERT_EQ(__real_fioGetCompletedRequest(ctx, &completed_requests), 1);
    ASSERT_EQ(completed_requests[0]->err_no, -ENOSYS);

    // event.res2 is not 0
    ctx->io_events[0].res2 = -EINVAL;
    setupMockLibAioReturnCodes(1, 0, 0, true);
    ctx->num_inflight_request = 1;
    ASSERT_EQ(__real_fioGetCompletedRequest(ctx, &completed_requests), 1);
    ASSERT_EQ(completed_requests[0]->err_no, -EINVAL);

    // Free the context event data
    fcFree(buf);

    // No real in-flight requests for clean-up
    ctx->num_inflight_request = 0;
    disableMockFio();
}

TEST_F(FioTest, testAccessModes) {
    enableMockFio(10, true, "");

    // Try to write to file with read only permissions.
    fioContext *context_read = fioCreateContext(ctx->filename, FILE_READ_ONLY, QUEUE_DEPTH, mockClockGetTimeUs);
    ASSERT_NE(context_read, nullptr);

    char buf[11] = "test write";
    ASSERT_EQ(-1, write(context_read->fd, buf, strlen(buf)));
    // Assert that the error is errno 9- "Bad file descriptor" indicating that the fd is not open for writing.
    ASSERT_STREQ("Bad file descriptor", strerror(errno));

    fioReleaseContext(context_read);

    // Test that fioContext is not created with an invalid mode.
    int bad_mode = 5;
    fioContext *context_wrong =  fioCreateContext(ctx->filename, bad_mode, QUEUE_DEPTH, mockClockGetTimeUs);
    ASSERT_EQ(context_wrong, nullptr);
    disableMockFio();
}
