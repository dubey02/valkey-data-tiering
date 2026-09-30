/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <atomic>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "server.h"
}

#ifdef USE_EXT_STORAGE

extern "C" {
#include "ext_storage.h"
#include "storage/storage.h"
#include "storage/storage_flashcache.h"
}

/* Records the thread that ran serialize, then forwards to the real one. */
static std::atomic<int> serialize_calls{0};
static pthread_t serialize_thread;
static int (*real_serialize)(const char *, size_t, const valkeyObject *, char **);
static int recordingSerialize(const char *key, size_t key_len, const valkeyObject *entry, char **out) {
    serialize_thread = pthread_self();
    serialize_calls++;
    return real_serialize(key, key_len, entry, out);
}

class StorageFlashCacheTest : public ::testing::Test {
  protected:
    MockValkey mock;
    RealValkey real;

    const storageEngine *engine = nullptr;
    storageConfig cfg = {};

    void SetUp() override {
        extStorageTestResetRegistry();
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
        server.logfile = zstrdup("");
        crc64_init();

        engine = storageFlashCacheEngine();
        cfg.path = "";
        cfg.capacity_bytes = 0;
        cfg.num_databases = 4;
        cfg.serializer = *extStorageSerializer();
    }

    void openEngine() {
        ASSERT_EQ(engine->open(&cfg), STORAGE_OK);
    }

    /* Polls until at least one completion arrives. Returns 0 if none arrive
     * within the retry budget. */
    int waitForCompletions(storageCompletion *out, int max = 1) {
        for (int retry = 0; retry < 50; retry++) {
            int n = engine->poll_completions(out, max);
            if (n > 0) return n;
            usleep(100 * 1000);
        }
        return 0;
    }

    storageStatus putAndWait(uint32_t db_id, sds key, const dbEntry *entry) {
        storageStatus submit = engine->put_async(db_id, key, sdslen(key), entry, nullptr);
        if (submit != STORAGE_OK) return submit;
        storageCompletion comp = {};
        EXPECT_EQ(waitForCompletions(&comp), 1);
        EXPECT_EQ(comp.op_type, STORAGE_OP_PUT);
        return comp.status;
    }

    void drainCompletions() {
        storageCompletion done[16];
        int n;
        while ((n = engine->poll_completions(done, 16)) > 0) {
            for (int i = 0; i < n; i++) {
                if (done[i].value) decrRefCount((robj *)done[i].value);
            }
        }
    }

    void TearDown() override {
        drainCompletions();
        engine->close();
        extStorageTestResetRegistry();
        zfree(server.logfile);
        server.logfile = NULL;
    }

    dbEntry *makeEntry(sds key, const char *val) {
        robj *o = createStringObject(val, strlen(val));
        return objectSetKeyAndExpire(o, key, EXPIRY_NONE);
    }
};

TEST_F(StorageFlashCacheTest, RejectsRequestsWhenNotOpen) {
    sds key = sdsnew("k");
    EXPECT_EQ(engine->get_async(0, key, sdslen(key), nullptr), STORAGE_ERR_REJECTED);
    storageCompletion comp = {};
    EXPECT_EQ(engine->poll_completions(&comp, 1), 0);
    sdsfree(key);
}

TEST_F(StorageFlashCacheTest, SerializesOnTheIoThread) {
    real_serialize = cfg.serializer.serialize;
    cfg.serializer.serialize = recordingSerialize;
    serialize_calls = 0;
    openEngine();

    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);
    EXPECT_EQ(serialize_calls.load(), 1);
    EXPECT_FALSE(pthread_equal(serialize_thread, pthread_self()));

    sdsfree(key);
    decrRefCount(entry);
}

TEST_F(StorageFlashCacheTest, TheKeyIsCopiedAtSubmit) {
    openEngine();
    sds key = sdsnew("lent");
    dbEntry *entry = makeEntry(key, "value");
    ASSERT_EQ(engine->put_async(0, key, sdslen(key), entry, nullptr), STORAGE_OK);
    sdsfree(key);

    storageCompletion comp = {};
    ASSERT_EQ(waitForCompletions(&comp), 1);
    EXPECT_EQ(comp.status, STORAGE_OK);
    decrRefCount(entry);
}

TEST_F(StorageFlashCacheTest, ReopenAfterCloseAcceptsRequests) {
    openEngine();
    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);
    engine->close();

    openEngine();
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);

    sdsfree(key);
    decrRefCount(entry);
}

TEST_F(StorageFlashCacheTest, CompletionCarriesTheRequestContext) {
    openEngine();
    int token = 42;
    sds key = sdsnew("k");
    ASSERT_EQ(engine->get_async(0, key, sdslen(key), &token), STORAGE_OK);
    storageCompletion comp = {};
    ASSERT_EQ(waitForCompletions(&comp), 1);
    EXPECT_EQ(comp.request_ctx, &token);
    EXPECT_EQ(comp.op_type, STORAGE_OP_GET);
    EXPECT_EQ(comp.db_id, 0u);
    sdsfree(key);
}

TEST_F(StorageFlashCacheTest, EveryRequestCompletesExactlyOnce) {
    openEngine();
    const int total = 5000;
    std::vector<int> seen(total, 0);
    std::vector<sds> keys(total);
    std::vector<dbEntry *> entries(total);
    for (int i = 0; i < total; i++) {
        keys[i] = sdscatprintf(sdsempty(), "key-%d", i);
        entries[i] = makeEntry(keys[i], "v");
    }

    storageCompletion done[64];
    int completed = 0;
    int would_block = 0;
    auto collect = [&]() {
        int n = waitForCompletions(done, 64);
        ASSERT_GT(n, 0) << "no completion arrived";
        for (int i = 0; i < n; i++) {
            ASSERT_EQ(done[i].status, STORAGE_OK);
            seen[(int)(intptr_t)done[i].request_ctx]++;
            completed++;
        }
    };

    for (int i = 0; i < total; i++) {
        storageStatus st;
        while ((st = engine->put_async(0, keys[i], sdslen(keys[i]), entries[i], (void *)(intptr_t)i)) ==
               STORAGE_WOULDBLOCK) {
            would_block++;
            collect();
        }
        ASSERT_EQ(st, STORAGE_OK);
    }
    while (completed < total) collect();
    ASSERT_EQ(completed, total);
    for (int i = 0; i < total; i++) EXPECT_EQ(seen[i], 1) << "request " << i;
    EXPECT_GT(would_block, 0) << "the test did not exercise the queue bound";

    for (int i = 0; i < total; i++) {
        sdsfree(keys[i]);
        decrRefCount(entries[i]);
    }
}

TEST_F(StorageFlashCacheTest, PartialPollsPreserveCompletionOrder) {
    openEngine();
    const int total = 20;
    sds key = sdsnew("k");
    for (int i = 0; i < total; i++) {
        ASSERT_EQ(engine->get_async(0, key, sdslen(key), (void *)(intptr_t)i), STORAGE_OK);
    }

    int seen = 0;
    storageCompletion out[3];
    while (seen < total) {
        int n = waitForCompletions(out, 3);
        ASSERT_GT(n, 0) << "no completion arrived";
        for (int i = 0; i < n; i++) {
            EXPECT_EQ((int)(intptr_t)out[i].request_ctx, seen) << "completion " << seen << " out of order";
            seen++;
        }
    }
    ASSERT_EQ(seen, total);
    sdsfree(key);
}

TEST_F(StorageFlashCacheTest, PollingAnIdleStorageEngineReturnsNothing) {
    openEngine();
    storageCompletion comp = {};
    EXPECT_EQ(engine->poll_completions(&comp, 1), 0);
}

TEST_F(StorageFlashCacheTest, ClosingWithQueuedRequestsLeaksNothing) {
    openEngine();
    const int n = 200;
    std::vector<sds> keys(n);
    std::vector<dbEntry *> entries(n);
    for (int i = 0; i < n; i++) {
        keys[i] = sdscatprintf(sdsempty(), "k%d", i);
        entries[i] = makeEntry(keys[i], "v");
        ASSERT_EQ(engine->put_async(0, keys[i], sdslen(keys[i]), entries[i], nullptr), STORAGE_OK);
    }
    engine->close();
    for (int i = 0; i < n; i++) {
        sdsfree(keys[i]);
        decrRefCount(entries[i]);
    }
    storageCompletion comp = {};
    EXPECT_EQ(engine->poll_completions(&comp, 1), 0);
}

#endif /* USE_EXT_STORAGE */
