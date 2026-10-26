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

#include <map>
#include <string>

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

/* A sink that keeps the reported fields by name. The storage engine reports
 * only unsigned counters today, so the other field kinds are not kept. */
typedef std::map<std::string, unsigned long long> StatMap;
static void sinkAddULongLong(void *c, const char *name, unsigned long long v) {
    (*(StatMap *)c)[name] = v;
}
static void sinkAddLongLong(void *, const char *, long long) {
}
static void sinkAddDouble(void *, const char *, double) {
}
static void sinkAddCString(void *, const char *, const char *) {
}
static void sinkBeginDict(void *, const char *) {
}
static void sinkEndDict(void *) {
}
static const storageInfoSink stat_map_sink_template = {
    nullptr, sinkAddLongLong, sinkAddULongLong, sinkAddDouble, sinkAddCString, sinkBeginDict, sinkEndDict};

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
    /* Data directory for this test, pid-unique so parallel test runs never
     * collide. The storage engine creates its db file inside it. */
    char fc_dir[256];
    char fc_db[300];

    void SetUp() override {
        extStorageTestResetRegistry();
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
        server.logfile = zstrdup("");
        crc64_init();
        monotonicInit();

        static int test_counter = 0;
        snprintf(fc_dir, sizeof(fc_dir), "/tmp/fc-unit-%d-%d", (int)getpid(), test_counter++);
        snprintf(fc_db, sizeof(fc_db), "%s/%s", fc_dir, STORAGE_FLASHCACHE_DB_FILE);
        removeDataDir();

        engine = storageFlashCacheEngine();
        cfg.path = fc_dir;
        cfg.capacity_bytes = 128 * 1024 * 1024; /* 128mb */
        cfg.num_databases = 4;
        cfg.serializer = *extStorageSerializer();
        cfg.thread_config = test_thread_config;
        cfg.logger = testLog;
        cfg.monotonic_clock_us = getMonotonicUs;
    }

    void removeDataDir() {
        unlink(fc_db);
        rmdir(fc_dir);
    }

    void openEngine() {
        ASSERT_EQ(engine->open(&cfg), STORAGE_OK);
    }

    /* Reads what the storage engine reports to INFO right now. */
    StatMap readStats() {
        StatMap m;
        storageInfoSink sink = stat_map_sink_template;
        sink.ctx = &m;
        engine->info(&sink);
        return m;
    }

    /* Waits until the named counter satisfies ok. The IO thread refreshes the
     * snapshot INFO reads on its own clock, so a change lands a little after
     * the request that caused it. Returns false when the wait runs out. */
    template <typename Pred>
    bool waitForStat(const char *name, Pred ok) {
        for (int retry = 0; retry < WAIT_RETRIES; retry++) {
            if (ok(readStats()[name])) return true;
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

    /* Reads one key and returns its completion. The caller owns comp.value on
     * STORAGE_OK and must decrRefCount it. */
    storageCompletion getAndWait(uint32_t db_id, sds key) {
        storageCompletion comp = {};
        EXPECT_EQ(engine->get_async(db_id, key, sdslen(key), nullptr), STORAGE_OK);
        EXPECT_EQ(waitForCompletions(&comp), 1);
        EXPECT_EQ(comp.op_type, STORAGE_OP_GET);
        return comp;
    }

    /* Deletes one key and returns its completion status. */
    storageStatus delAndWait(uint32_t db_id, sds key) {
        storageCompletion comp = {};
        EXPECT_EQ(engine->del_async(db_id, key, sdslen(key), nullptr), STORAGE_OK);
        EXPECT_EQ(waitForCompletions(&comp), 1);
        EXPECT_EQ(comp.op_type, STORAGE_OP_DEL);
        return comp.status;
    }

    /* Polls until close accepts, so a test that failed early cannot leave a
     * request in flight. Close refuses with STORAGE_WOULDBLOCK while anything
     * is unpolled. Gives up after five seconds. */
    void drainAndClose() {
        storageCompletion done[16];
        storageStatus st;
        for (int waited_us = 0; (st = engine->close()) == STORAGE_WOULDBLOCK && waited_us < 5000000;) {
            int n = engine->poll_completions(done, 16);
            for (int i = 0; i < n; i++) {
                if (done[i].value) decrRefCount((robj *)done[i].value);
            }
            if (n == 0) {
                usleep(WAIT_STEP_US);
                waited_us += WAIT_STEP_US;
            }
        }
        EXPECT_EQ(st, STORAGE_OK) << "close did not accept within five seconds";
    }

    void TearDown() override {
        drainAndClose();
        extStorageTestResetRegistry();
        zfree(server.logfile);
        server.logfile = NULL;
        removeDataDir();
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
    ASSERT_EQ(engine->close(), STORAGE_OK);

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

/* Completions come out in submission order when polled a few at a time. A
 * delete is queued last, so it arrives after every get. */
TEST_F(StorageFlashCacheTest, PartialPollsPreserveCompletionOrder) {
    openEngine();
    const int total = 20;
    sds key = sdsnew("k");
    for (int i = 0; i < total; i++) {
        ASSERT_EQ(engine->get_async(0, key, sdslen(key), (void *)(intptr_t)i), STORAGE_OK);
    }
    ASSERT_EQ(engine->del_async(0, key, sdslen(key), (void *)(intptr_t)total), STORAGE_OK);

    int seen = 0;
    bool saw_del = false;
    storageCompletion out[3];
    while (!saw_del) {
        int n = waitForCompletions(out, 3);
        ASSERT_GT(n, 0) << "no completion arrived";
        for (int i = 0; i < n; i++) {
            EXPECT_EQ((int)(intptr_t)out[i].request_ctx, seen) << "completion " << seen << " out of order";
            if (out[i].op_type == STORAGE_OP_DEL) saw_del = true;
            seen++;
        }
    }
    EXPECT_EQ(seen, total + 1);
    sdsfree(key);
}

/* Close after every put was polled returns the memory to where it started. */
TEST_F(StorageFlashCacheTest, ClosingAfterPollingLeaksNothing) {
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
    int polled = 0;
    storageCompletion done[64];
    while (polled < n) {
        int got = waitForCompletions(done, 64);
        ASSERT_GT(got, 0) << "no completion arrived";
        polled += got;
    }
    ASSERT_EQ(polled, n);
    ASSERT_EQ(engine->close(), STORAGE_OK);
    for (int i = 0; i < n; i++) {
        sdsfree(keys[i]);
        decrRefCount(entries[i]);
    }
    storageCompletion comp = {};
    EXPECT_EQ(engine->poll_completions(&comp, 1), 0);
    EXPECT_EQ(zmalloc_used_memory(), used_before);
}

/* Close is refused while a request is unpolled and the storage engine stays
 * open. Once the request is polled, close goes through. */
TEST_F(StorageFlashCacheTest, CloseIsRefusedWhileARequestIsUnpolled) {
    openEngine();
    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");
    ASSERT_EQ(engine->put_async(0, key, sdslen(key), entry, nullptr), STORAGE_OK);
    EXPECT_EQ(engine->close(), STORAGE_WOULDBLOCK);

    storageCompletion comp = {};
    ASSERT_EQ(waitForCompletions(&comp), 1);
    EXPECT_EQ(comp.status, STORAGE_OK);
    EXPECT_EQ(engine->close(), STORAGE_OK);

    sdsfree(key);
    decrRefCount(entry);
}

/* A put, then a get of the same key, returns the stored value. This is the
 * spill then fetch round trip against real FlashCache. */
TEST_F(StorageFlashCacheTest, PutThenGetReturnsTheStoredValue) {
    openEngine();
    sds key = sdsnew("round-trip-key");
    dbEntry *entry = makeEntry(key, "the-stored-value");
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);

    storageCompletion comp = getAndWait(0, key);
    ASSERT_EQ(comp.status, STORAGE_OK);
    ASSERT_NE(comp.value, nullptr);
    robj *want = createStringObject("the-stored-value", strlen("the-stored-value"));
    EXPECT_TRUE(equalStringObjects((robj *)comp.value, want));
    decrRefCount(want);
    decrRefCount((robj *)comp.value);

    sdsfree(key);
    decrRefCount(entry);
}

/* A get of a key that was never put returns not found and no value. */
TEST_F(StorageFlashCacheTest, GetOfAMissingKeyReturnsNotFound) {
    openEngine();
    sds key = sdsnew("never-written");
    storageCompletion comp = getAndWait(0, key);
    EXPECT_EQ(comp.status, STORAGE_NOT_FOUND);
    EXPECT_EQ(comp.value, nullptr);
    sdsfree(key);
}

/* A put, a delete, then a get of the same key returns not found. */
TEST_F(StorageFlashCacheTest, DeleteRemovesTheStoredValue) {
    openEngine();
    sds key = sdsnew("to-delete");
    dbEntry *entry = makeEntry(key, "v");
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);

    ASSERT_EQ(delAndWait(0, key), STORAGE_OK);

    storageCompletion comp = getAndWait(0, key);
    EXPECT_EQ(comp.status, STORAGE_NOT_FOUND);
    EXPECT_EQ(comp.value, nullptr);

    sdsfree(key);
    decrRefCount(entry);
}

/* A second put on the same key overwrites, so the get returns the newer value. */
TEST_F(StorageFlashCacheTest, PutOverwritesAnExistingKey) {
    openEngine();
    sds key = sdsnew("overwrite");
    dbEntry *first = makeEntry(key, "first");
    dbEntry *second = makeEntry(key, "second");
    ASSERT_EQ(putAndWait(0, key, first), STORAGE_OK);
    ASSERT_EQ(putAndWait(0, key, second), STORAGE_OK);

    storageCompletion comp = getAndWait(0, key);
    ASSERT_EQ(comp.status, STORAGE_OK);
    ASSERT_NE(comp.value, nullptr);
    robj *want = createStringObject("second", strlen("second"));
    EXPECT_TRUE(equalStringObjects((robj *)comp.value, want));
    decrRefCount(want);
    decrRefCount((robj *)comp.value);

    sdsfree(key);
    decrRefCount(first);
    decrRefCount(second);
}

/* The same key in two databases is two records. A put in one does not appear
 * in the other. */
TEST_F(StorageFlashCacheTest, DatabasesAreIsolated) {
    openEngine();
    sds key = sdsnew("shared-name");
    dbEntry *entry = makeEntry(key, "in-db-0");
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);

    storageCompletion in_db1 = getAndWait(1, key);
    EXPECT_EQ(in_db1.status, STORAGE_NOT_FOUND);
    EXPECT_EQ(in_db1.value, nullptr);

    storageCompletion in_db0 = getAndWait(0, key);
    ASSERT_EQ(in_db0.status, STORAGE_OK);
    ASSERT_NE(in_db0.value, nullptr);
    decrRefCount((robj *)in_db0.value);

    sdsfree(key);
    decrRefCount(entry);
}

/* A binary value with embedded NULs and the full byte range survives the round
 * trip, so the length is carried, not a C string. */
TEST_F(StorageFlashCacheTest, BinaryValueSurvivesTheRoundTrip) {
    openEngine();
    char raw[256];
    for (int i = 0; i < 256; i++) raw[i] = (char)i;
    sds key = sdsnew("binary");
    robj *o = createStringObject(raw, sizeof(raw));
    dbEntry *entry = objectSetKeyAndExpire(o, key, EXPIRY_NONE);
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);

    storageCompletion comp = getAndWait(0, key);
    ASSERT_EQ(comp.status, STORAGE_OK);
    ASSERT_NE(comp.value, nullptr);
    robj *want = createStringObject(raw, sizeof(raw));
    EXPECT_TRUE(equalStringObjects((robj *)comp.value, want));
    decrRefCount(want);
    decrRefCount((robj *)comp.value);

    sdsfree(key);
    decrRefCount(entry);
}

/* The info callback reports FlashCache's own counters from the snapshot the IO
 * thread refreshes. A put raises the items held and the bytes used, and a
 * delete does not raise them. The checks are one-sided because FlashCache
 * background work can move a live gauge. Nothing here can be throttled or
 * evicted, so those counters stay at zero. */
TEST_F(StorageFlashCacheTest, InfoReportsTheItemsHeld) {
    openEngine();
    StatMap empty = readStats();
    EXPECT_EQ(empty["total_num_items"], 0u);
    EXPECT_EQ(empty["total_num_requests_throttled"], 0u);

    sds key = sdsnew("counted");
    dbEntry *entry = makeEntry(key, "v");
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);
    EXPECT_TRUE(waitForStat("total_num_items", [](unsigned long long v) { return v >= 1; }));
    StatMap after_put = readStats();
    EXPECT_GE(after_put["total_num_bytes"], 1u);

    ASSERT_EQ(delAndWait(0, key), STORAGE_OK);
    unsigned long long held = after_put["total_num_items"];
    EXPECT_TRUE(waitForStat("total_num_items", [held](unsigned long long v) { return v <= held; }));
    StatMap after_del = readStats();
    EXPECT_EQ(after_del["total_num_requests_throttled"], 0u);
    EXPECT_EQ(after_del["total_num_items_evicted"], 0u);

    sdsfree(key);
    decrRefCount(entry);
}

/* A value larger than one FlashCache block round trips whole. This drives
 * FlashCache's own chunking of a record across blocks. */
TEST_F(StorageFlashCacheTest, LargeValueSurvivesTheRoundTrip) {
    openEngine();
    const size_t big = 4 * 1024 * 1024; /* 4mb, well past one block */
    sds blob = sdsnewlen(NULL, big);
    for (size_t i = 0; i < big; i++) blob[i] = (char)(i * 31 + 7);
    sds key = sdsnew("large");
    robj *o = createStringObject(blob, big);
    dbEntry *entry = objectSetKeyAndExpire(o, key, EXPIRY_NONE);
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);

    storageCompletion comp = getAndWait(0, key);
    ASSERT_EQ(comp.status, STORAGE_OK);
    ASSERT_NE(comp.value, nullptr);
    robj *want = createStringObject(blob, big);
    EXPECT_TRUE(equalStringObjects((robj *)comp.value, want));
    decrRefCount(want);
    decrRefCount((robj *)comp.value);

    sdsfree(blob);
    sdsfree(key);
    decrRefCount(entry);
}

/* A non-string type round trips. The DUMP codec carries the type, so the
 * fetched object is a set with the same members, not a string. This proves the
 * round trip is not string-only. */
TEST_F(StorageFlashCacheTest, NonStringTypeSurvivesTheRoundTrip) {
    openEngine();
    robj *set = createSetObject();
    setTypeAddAux(set, (char *)"alpha", 5, 0, 0);
    setTypeAddAux(set, (char *)"beta", 4, 0, 0);
    setTypeAddAux(set, (char *)"gamma", 5, 0, 0);
    unsigned long want_size = setTypeSize(set);
    sds key = sdsnew("a-set");
    dbEntry *entry = objectSetKeyAndExpire(set, key, EXPIRY_NONE);
    ASSERT_EQ(putAndWait(0, key, entry), STORAGE_OK);

    storageCompletion comp = getAndWait(0, key);
    ASSERT_EQ(comp.status, STORAGE_OK);
    ASSERT_NE(comp.value, nullptr);
    robj *got = (robj *)comp.value;
    EXPECT_EQ(got->type, OBJ_SET);
    EXPECT_EQ(setTypeSize(got), want_size);
    decrRefCount(got);

    sdsfree(key);
    decrRefCount(entry);
}

#endif /* USE_EXT_STORAGE */
