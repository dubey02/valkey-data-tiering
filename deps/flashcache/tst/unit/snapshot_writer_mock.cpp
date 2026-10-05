#include <cstdio>
#include "snapshot_writer_mock.hpp"
#include "flashcache_test_base.hpp"
#include "clock_mock.hpp"

extern "C" {
#include "include/snapshot_version_one.h"
#include "include/fio.h"
#include "include/util.h"
}

char snapshot_filename[FILENAME_MAX];
fioContext *snapshot_file_io_context = NULL;
size_t num_set_snapshot_size_invocation = 0;  // Number of times set_snapshot_size callback has been invoked
size_t num_is_writable_invocation = 0;  // Number of times is_writable callback has been invoked
size_t num_write_invocation = 0;  // Number of times write callback has been invoked
size_t num_complete_invocation = 0;  // Number of times complete callback has been invoked
size_t snapshot_size_bytes = 0;
size_t snapshot_written_bytes = 0;
size_t num_keep_alive_invocation = 0;

static void setSnapshotSizeMock(void *context, size_t size) {
    (void(context));
    snapshot_file_io_context =
            fioCreateContext(snapshot_filename, FILE_READ_WRITE, FC_SNAPSHOT_QUEUE_DEPTH, mockClockGetTimeUs);
    num_set_snapshot_size_invocation++;
    snapshot_size_bytes = size;
    snapshot_written_bytes = 0;
}

static void writeMock(void *context, size_t offset, char *buf, size_t buf_len) {
    (void(context));

    fioRequest *fio_request = reinterpret_cast<fioRequest *>(fcMalloc(sizeof(fioRequest)));
    fioRequestFill(fio_request, 0, buf, buf_len, offset, FC_FIO_WRITE);
    fioSubmit(snapshot_file_io_context, fio_request);

    fioRequest **completed_fio_requests = NULL;
    size_t minRequestProcessingDelay = mockFioGetMinRequestProcessingDelay();  // Capture before changing it.
    mockFioSetMinRequestProcessingDelay(0);  // Setting request processing delay to 0
    // Wait for the write request to complete
    while (fioGetCompletedRequest(snapshot_file_io_context, &completed_fio_requests) != 1) {
        // Busy spin
    }
    // Reverting request processing delay to its prev value
    mockFioSetMinRequestProcessingDelay(minRequestProcessingDelay);
    num_write_invocation++;
    free(buf);  // We have already removed size of buf from current_usage_memory, hence not using fcFree.
    fcFree(fio_request);
    snapshot_written_bytes += buf_len;
}

static int isWritableMock(void *context) {
    (void(context));
    return ((num_is_writable_invocation++) % 2);
}

static void writeKeepAlive(void *context) {
    (void(context));
    num_keep_alive_invocation++;
}

static void completeMock(void *context, int completed) {
    fioReleaseContext(snapshot_file_io_context);
    snapshot_file_io_context = NULL;
    num_complete_invocation++;
    snapshotContext *snapshot_context = reinterpret_cast<snapshotContext *>(context);
    snapshot_context->num_snapshot_completion_callback_invocation++;
    ASSERT_EQ(snapshot_context->expected_completion_status, completed);
    if (completed == 1) {
        // In THREADSAVE replication, the snapshot contains EOF, replication cmds
        // and some delete requests in unprocessed snapshotting range. Because of
        // this, snapshot_written_bytes can not be predicted as it can be more/less
        // or equal to snapshot_size_bytes.
        if (snapshot_context->snapshot_save_type == FC_SAVE_TYPE_FORKLESS_SAVE) {
            ASSERT_GE(snapshot_written_bytes, 0);
        } else {
            // In the case of snapshot v2 BGSAVE, we can have a situation where
            // snapshot_file_data_size_bytes is a multiple of PAGESIZE and EOF will
            // take up a whole new page, so snapshot_written_bytes might be equal to
            // snapshot_size_bytes or there can be a difference of 1 page due to EOF.
            if (snapshot_context->snapshot_version == FC_SNAPSHOT_VERSION_TWO) {
                ASSERT_TRUE(
                    snapshot_written_bytes == snapshot_size_bytes ||
                    snapshot_written_bytes == getCeilPageAlignedOffset(snapshot_size_bytes + getEOFItemSizeBytes()));
            } else {
                ASSERT_EQ(snapshot_written_bytes, snapshot_size_bytes);
            }
        }
    } else {
        ASSERT_LT(snapshot_written_bytes, snapshot_size_bytes);
    }
}

void resetSnapshotWriterCounters() {
    num_set_snapshot_size_invocation = 0;
    num_is_writable_invocation = 0;
    num_write_invocation = 0;
    num_keep_alive_invocation = 0;
    num_complete_invocation = 0;
    snapshot_size_bytes = 0;
    snapshot_written_bytes = 0;
}

void initializeSnapshotWriterMock(char const *filename) {
    snprintf(snapshot_filename, FILENAME_MAX, "%s", filename);
    snapshot_file_io_context = NULL;
    resetSnapshotWriterCounters();
}

size_t snapshotWriterMockGetNumSetSnapshotSizeInvocation() {
    return num_set_snapshot_size_invocation;
}

size_t snapshotWriterMockGetNumIsWritableInvocation() {
    return num_is_writable_invocation;
}

size_t snapshotWriterMockGetNumWriteInvocation() {
    return num_write_invocation;
}

size_t snapshotWriterMockGetNumCompleteInvocation() {
    return num_complete_invocation;
}

flashcacheSnapshotWriter *getMockSnapshotWriter() {
    flashcacheSnapshotWriter *snapshot_writer = new flashcacheSnapshotWriter();
    snapshot_writer->set_snapshot_size = &setSnapshotSizeMock;
    snapshot_writer->write = &writeMock;
    snapshot_writer->is_writable = &isWritableMock;
    snapshot_writer->keep_alive = &writeKeepAlive;
    snapshot_writer->complete = &completeMock;
    return snapshot_writer;
}

void releaseMockSnapshotWriter(flashcacheSnapshotWriter *snapshot_writer) {
    if (snapshot_writer != NULL) {
        delete snapshot_writer;
    }
}
