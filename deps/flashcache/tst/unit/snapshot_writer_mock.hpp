#ifndef __FLASHCACHE_SNAPSHOT_WRITER_MOCK_HPP
#define __FLASHCACHE_SNAPSHOT_WRITER_MOCK_HPP

extern "C" {
#include "include/flashcache_common.h"
}

void initializeSnapshotWriterMock(char const *filename);
size_t snapshotWriterMockGetNumSetSnapshotSizeInvocation();
size_t snapshotWriterMockGetNumIsWritableInvocation();
size_t snapshotWriterMockGetNumWriteInvocation();
size_t snapshotWriterMockGetNumCompleteInvocation();
flashcacheSnapshotWriter *getMockSnapshotWriter();
void resetSnapshotWriterCounters();
void releaseMockSnapshotWriter(flashcacheSnapshotWriter *snapshot_writer);
#endif  // __FLASHCACHE_SNAPSHOT_WRITER_MOCK_HPP
