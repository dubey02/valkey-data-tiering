/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include "server.h"
}

#ifdef USE_EXT_STORAGE

extern "C" {
#include "ext_storage.h"
#include "storage/storage.h"
#include "storage/storage_flashcache.h"
void extStorageTestResetRegistry(void);
}

/* The tests own the log and the thread setup. The log goes to stderr. The
 * thread gets the stack the serializer needs and nothing else. */
static void testLog(storageLogLevel level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[storage %d] ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const storageThreadConfig test_thread_config = {VALKEY_THREAD_STACK_SIZE, NULL};

#define WAIT_STEP_US 1000
#define WAIT_RETRIES 5000

/* Records the thread that ran serialize and the key it was handed, then
 * forwards to the real one. */
static int serialize_calls = 0;
static pthread_t serialize_thread;
static const char *serialize_key_ptr;
static char serialize_key[16];
static int (*real_serialize)(const char *, size_t, const valkeyObject *, char **);
static int recordingSerialize(const char *key, size_t key_len, const valkeyObject *entry, char **out) {
    serialize_thread = pthread_self();
    serialize_calls++;
    serialize_key_ptr = key;
    snprintf(serialize_key, sizeof(serialize_key), "%.*s", (int)key_len, key);
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
        cfg.thread_config = test_thread_config;
        cfg.logger = testLog;
    }

    void openEngine() {
        ASSERT_EQ(engine->open(&cfg), STORAGE_OK);
    }

    /* Waits until n completions are queued for the next poll. Returns false if
     * the storage engine does not get there within the retry budget. */
    bool waitForPendingCompletions(int n) {
        storageStats st;
        for (int retry = 0; retry < WAIT_RETRIES; retry++) {
            engine->get_stats(&st);
            if (st.num_completions_pending >= (uint64_t)n) return true;
            usleep(WAIT_STEP_US);
        }
        return false;
    }

    /* Polls until at least one completion arrives. Returns 0 if none arrive
     * within the retry budget. */
    int waitForCompletions(storageCompletion *out, int max = 1) {
        for (int retry = 0; retry < WAIT_RETRIES; retry++) {
            int n = engine->poll_completions(out, max);
            if (n > 0) return n;
            usleep(WAIT_STEP_US);
        }
        return 0;
    }

    /* Takes whatever has completed and records each request once. */
    void collectCompletions(int *seen, int *completed) {
        storageCompletion done[64];
        int n = waitForCompletions(done, 64);
        ASSERT_GT(n, 0) << "no completion arrived";
        for (int i = 0; i < n; i++) {
            ASSERT_EQ(done[i].status, STORAGE_OK);
            seen[(int)(intptr_t)done[i].request_ctx]++;
            (*completed)++;
        }
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

/* The last line the storage engine logged, and at what level. */
static char logged_msg[256];
static int logged_level = -1;
static void recordingLog(storageLogLevel level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(logged_msg, sizeof(logged_msg), fmt, ap);
    va_end(ap);
    logged_level = level;
}

TEST_F(StorageFlashCacheTest, RejectsAndLogsAnUnknownOption) {
    storageOption opts[] = {{"no-such-option", "1"}};
    cfg.options = opts;
    cfg.num_options = 1;
    cfg.logger = recordingLog;
    logged_msg[0] = '\0';
    logged_level = -1;

    EXPECT_EQ(engine->open(&cfg), STORAGE_ERR_REJECTED);
    EXPECT_EQ(logged_level, STORAGE_LOG_WARNING);
    EXPECT_NE(strstr(logged_msg, "no-such-option"), nullptr);
}

TEST_F(StorageFlashCacheTest, RejectsABadDbIdAtSubmit) {
    openEngine();
    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");
    EXPECT_EQ(engine->put_async(cfg.num_databases, key, sdslen(key), entry, nullptr), STORAGE_ERR_REJECTED);
    EXPECT_EQ(engine->get_async(cfg.num_databases, key, sdslen(key), nullptr), STORAGE_ERR_REJECTED);
    EXPECT_EQ(engine->del_async(cfg.num_databases, key, sdslen(key), nullptr), STORAGE_ERR_REJECTED);
    storageCompletion comp = {};
    EXPECT_EQ(engine->poll_completions(&comp, 1), 0);
    sdsfree(key);
    decrRefCount(entry);
}

TEST_F(StorageFlashCacheTest, SerializesOnTheIoThread) {
    real_serialize = cfg.serializer.serialize;
    cfg.serializer.serialize = recordingSerialize;
    serialize_calls = 0;
    openEngine();

    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);
    EXPECT_EQ(serialize_calls, 1);
    EXPECT_FALSE(pthread_equal(serialize_thread, pthread_self()));

    sdsfree(key);
    decrRefCount(entry);
}

/* Which thread ran the hook, and how many serialize calls came before it. */
static pthread_t policy_thread;
static int serialize_calls_at_policy = -1;
static void recordingThreadStart(const char *name) {
    (void)name;
    policy_thread = pthread_self();
    serialize_calls_at_policy = serialize_calls;
}

TEST_F(StorageFlashCacheTest, RunsTheThreadPolicyFirstOnTheIoThread) {
    real_serialize = cfg.serializer.serialize;
    cfg.serializer.serialize = recordingSerialize;
    cfg.thread_config.on_thread_start = recordingThreadStart;
    serialize_calls = 0;
    serialize_calls_at_policy = -1;
    openEngine();

    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);
    EXPECT_EQ(serialize_calls_at_policy, 0);
    EXPECT_TRUE(pthread_equal(policy_thread, serialize_thread));
    EXPECT_FALSE(pthread_equal(policy_thread, pthread_self()));

    sdsfree(key);
    decrRefCount(entry);
}

TEST_F(StorageFlashCacheTest, TheKeyIsCopiedAtSubmit) {
    real_serialize = cfg.serializer.serialize;
    cfg.serializer.serialize = recordingSerialize;
    openEngine();
    sds key = sdsnew("lent");
    dbEntry *entry = makeEntry(key, "value");
    ASSERT_EQ(engine->put_async(0, key, sdslen(key), entry, nullptr), STORAGE_OK);
    sdsfree(key);

    storageCompletion comp = {};
    ASSERT_EQ(waitForCompletions(&comp), 1);
    EXPECT_EQ(comp.status, STORAGE_OK);
    /* The copy is made while key is still live, so a borrowed key would
     * reach the IO thread at the same address no matter when it runs. */
    EXPECT_NE(serialize_key_ptr, (const char *)key);
    EXPECT_STREQ(serialize_key, "lent");
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
    static int seen[total];
    static sds keys[total];
    static dbEntry *entries[total];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < total; i++) {
        keys[i] = sdscatprintf(sdsempty(), "key-%d", i);
        entries[i] = makeEntry(keys[i], "v");
    }

    int completed = 0;
    int would_block = 0;
    for (int i = 0; i < total; i++) {
        storageStatus st;
        while ((st = engine->put_async(0, keys[i], sdslen(keys[i]), entries[i], (void *)(intptr_t)i)) ==
               STORAGE_WOULDBLOCK) {
            would_block++;
            ASSERT_NO_FATAL_FAILURE(collectCompletions(seen, &completed));
        }
        ASSERT_EQ(st, STORAGE_OK);
    }
    while (completed < total) ASSERT_NO_FATAL_FAILURE(collectCompletions(seen, &completed));
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
    /* With every completion queued, a 3-wide poll must put some back. */
    ASSERT_TRUE(waitForPendingCompletions(total)) << "the IO thread did not finish";

    int seen = 0;
    storageCompletion out[3];
    int n = engine->poll_completions(out, 3);
    ASSERT_EQ(n, 3) << "the poll limit was not reached";
    while (true) {
        for (int i = 0; i < n; i++) {
            EXPECT_EQ((int)(intptr_t)out[i].request_ctx, seen) << "completion " << seen << " out of order";
            seen++;
        }
        if (seen >= total) break;
        n = waitForCompletions(out, 3);
        ASSERT_GT(n, 0) << "no completion arrived";
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
    size_t used_before = zmalloc_used_memory();
    openEngine();
    const int n = 200;
    sds keys[n];
    dbEntry *entries[n];
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
    EXPECT_EQ(zmalloc_used_memory(), used_before);
}

#endif /* USE_EXT_STORAGE */
