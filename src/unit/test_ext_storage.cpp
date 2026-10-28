/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>
#include <map>
#include <stdarg.h>
#include <stdio.h>
#include <string>

extern "C" {
#include "server.h"
}

#ifdef USE_EXT_STORAGE

extern "C" {
#include "ext_storage.h"
#include "module.h"
#include "storage/storage.h"
#include "storage/storage_mock.h"
void extStorageTestResetRegistry(void);
extern hashtableType commandSetType;
void createSharedObjects(void);
storageState getStorageState(const robj *o);
void setStorageState(robj *o, storageState state);
}

/* The log and the thread setup. */
static void testLog(storageLogLevel level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[storage %d] ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const storageThreadConfig test_thread_config = {};

/* A sink that keeps the reported fields by name. The mock reports only
 * unsigned counters, so the other field kinds are not kept. */
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

/* ---------------------------------------------------------------------------
 * Mock storage engine
 *
 * These tests drive the mock storage engine with the real serializer.
 * ---------------------------------------------------------------------------*/

class StorageMockTest : public ::testing::Test {
  protected:
    MockValkey mock;
    RealValkey real;

    const storageEngine *engine = nullptr;
    storageConfig cfg = {};

    void SetUp() override {
        extStorageTestResetRegistry();
        /* The real value serde uses the CRC64 table and logs through
         * server.logfile. Both must be initialized. */
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
        server.logfile = zstrdup("");
        crc64_init();

        engine = storageMockEngine();
        cfg.path = "";
        cfg.capacity_bytes = 0; /* unbounded unless a test overrides it */
        cfg.num_databases = 4;
        cfg.serializer = *extStorageSerializer();
        cfg.thread_config = test_thread_config;
        cfg.logger = testLog;
    }

    void openEngine() {
        ASSERT_EQ(engine->open(&cfg), STORAGE_OK);
    }

    /* Waits for one completed request. The mock storage engine is async only, so a
     * test submits a request and then waits for its completion. Returns
     * the number of completions fetched into out, 0 if none within max_ticks. */
    int waitForCompletedRequest(storageCompletion *out, int max_ticks = 8) {
        int fetched = 0;
        int retry = max_ticks;
        while (!fetched && retry) {
            storageMockRunIo(1);
            fetched = engine->poll_completions(out, 1);
            retry--;
        }
        return fetched;
    }

    /* Single-operation convenience wrappers. Each submits one async request and
     * waits for its completion, which is what a test does repeatedly against a
     * real async storage engine. A test that wants to observe the intermediate
     * states drives put_async, storageMockRunIo, and poll_completions directly.
     */

    /* Puts a value and returns the completion status. stored_bytes, when not
     * NULL, receives the serialized size. */
    storageStatus putAndWait(uint32_t db_id, sds key, const dbEntry *entry, size_t *stored_bytes) {
        storageStatus submit = engine->put_async(db_id, key, sdslen(key), entry, nullptr);
        if (submit != STORAGE_OK) return submit;
        storageCompletion comp = {};
        EXPECT_EQ(waitForCompletedRequest(&comp), 1);
        if (stored_bytes) *stored_bytes = comp.stored_bytes;
        return comp.status;
    }

    /* Gets a value. On STORAGE_OK, *out receives the value object the caller
     * must release. */
    storageStatus getAndWait(uint32_t db_id, sds key, robj **out) {
        *out = nullptr;
        storageStatus submit = engine->get_async(db_id, key, sdslen(key), nullptr);
        if (submit != STORAGE_OK) return submit;
        storageCompletion comp = {};
        EXPECT_EQ(waitForCompletedRequest(&comp), 1);
        if (comp.status == STORAGE_OK) *out = (robj *)comp.value;
        return comp.status;
    }

    /* Deletes a key and returns the completion status. */
    storageStatus delAndWait(uint32_t db_id, sds key) {
        storageStatus submit = engine->del_async(db_id, key, sdslen(key), nullptr);
        if (submit != STORAGE_OK) return submit;
        storageCompletion comp = {};
        EXPECT_EQ(waitForCompletedRequest(&comp), 1);
        return comp.status;
    }

    /* Reads what the storage engine reports to INFO. The mock runs on this
     * thread, so the counters are current. */
    StatMap readStats() {
        StatMap m;
        storageInfoSink sink = stat_map_sink_template;
        sink.ctx = &m;
        engine->info(&sink);
        return m;
    }

    /* Drains the inflight request. */
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
        EXPECT_EQ(engine->close(), STORAGE_OK);
        extStorageTestResetRegistry();
        zfree(server.logfile);
        server.logfile = NULL;
    }

    /* Serialized DUMP length of a string value, for byte-accounting asserts. */
    size_t serializedLen(const char *s) {
        robj *o = createStringObject(s, strlen(s));
        char *bytes = nullptr;
        int len = cfg.serializer.serialize("k", 1, o, &bytes);
        cfg.serializer.free_serialized(bytes);
        decrRefCount(o);
        return (size_t)len;
    }

    /* A keyspace entry, which is what put_async receives. The key is embedded in
     * the object, so this exercises the value accessors that branch on the
     * embedded key, expire, and value. */
    dbEntry *makeEntry(sds key, const char *val, long long expire = EXPIRY_NONE) {
        robj *o = createStringObject(val, strlen(val));
        return objectSetKeyAndExpire(o, key, expire);
    }

    dbEntry *makeIntEntry(sds key, long long val) {
        robj *o = createStringObjectFromLongLong(val);
        return objectSetKeyAndExpire(o, key, EXPIRY_NONE);
    }
};

TEST_F(StorageMockTest, ARegisteredEngineIsFoundByName) {
    ASSERT_EQ(storageRegisterEngine(storageMockEngine()), STORAGE_OK);
    ASSERT_NE(storageLookupEngine("mock"), nullptr);
}

TEST_F(StorageMockTest, RejectsAnEngineWithAMismatchedApiVersion) {
    storageEngine bad = *storageMockEngine();
    bad.api_version = VALKEY_STORAGE_API_VERSION + 1;
    EXPECT_EQ(storageRegisterEngine(&bad), STORAGE_ERR_REJECTED);
    EXPECT_EQ(storageLookupEngine("mock"), nullptr);
}

TEST_F(StorageMockTest, RejectsASecondEngine) {
    ASSERT_EQ(storageRegisterEngine(storageMockEngine()), STORAGE_OK);
    EXPECT_EQ(storageRegisterEngine(storageMockEngine()), STORAGE_ERR_REJECTED);
}

TEST_F(StorageMockTest, RefusesToOpenWithoutSerializer) {
    storageConfig missing_serialize = cfg;
    missing_serialize.serializer.serialize = nullptr;
    EXPECT_EQ(engine->open(&missing_serialize), STORAGE_ERR_REJECTED);

    storageConfig missing_deserialize = cfg;
    missing_deserialize.serializer.deserialize = nullptr;
    EXPECT_EQ(engine->open(&missing_deserialize), STORAGE_ERR_REJECTED);

    storageConfig missing_free = cfg;
    missing_free.serializer.free_serialized = nullptr;
    EXPECT_EQ(engine->open(&missing_free), STORAGE_ERR_REJECTED);
}

TEST_F(StorageMockTest, RejectsAnUnknownOption) {
    storageOption opts[] = {{"no-such-option", "1"}};
    cfg.options = opts;
    cfg.num_options = 1;
    EXPECT_EQ(engine->open(&cfg), STORAGE_ERR_REJECTED);
}

TEST_F(StorageMockTest, RejectsABadDbIdAtSubmit) {
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

TEST_F(StorageMockTest, PutGetDelRoundTrip) {
    openEngine();
    sds key = sdsnew("k1");
    dbEntry *entry = makeEntry(key, "hello");
    size_t stored = 0;

    ASSERT_EQ(putAndWait(0, key, entry, &stored), STORAGE_OK);
    EXPECT_EQ(stored, serializedLen("hello"));

    robj *out = nullptr;
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal(out), "hello"), 0);
    /* A fetched value is keyless */
    EXPECT_EQ(objectGetKey(out), nullptr);
    decrRefCount(out);

    /* The get consumed the record. */
    EXPECT_EQ(readStats()["total_num_items"], 0u);
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_NOT_FOUND);

    ASSERT_EQ(putAndWait(0, key, entry, &stored), STORAGE_OK);
    ASSERT_EQ(delAndWait(0, key), STORAGE_OK);
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_NOT_FOUND);
    /* A delete of a missing record is still OK. */
    EXPECT_EQ(delAndWait(0, key), STORAGE_OK);

    sdsfree(key);
    decrRefCount(entry);
}

/* A keyspace entry carries its key inside the object, and for a short string the
 * value is embedded in the same allocation. Both make the value accessors take
 * branches a keyless object does not. */
TEST_F(StorageMockTest, EmbeddedKeyEntryRoundTrips) {
    openEngine();
    sds key = sdsnew("embedded");
    dbEntry *entry = makeEntry(key, "value");

    ASSERT_NE(objectGetKey(entry), nullptr);
    EXPECT_EQ(sdscmp(objectGetKey(entry), key), 0);
    ASSERT_EQ(entry->hasembval, 1);

    ASSERT_EQ(putAndWait(0, key, entry, nullptr), STORAGE_OK);

    robj *out = nullptr;
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal(out), "value"), 0);
    decrRefCount(out);

    sdsfree(key);
    decrRefCount(entry);
}

/* An entry with an expire shifts the offset of every field that follows it. */
TEST_F(StorageMockTest, EntryWithAnExpireRoundTrips) {
    openEngine();
    sds key = sdsnew("volatile");
    dbEntry *entry = makeEntry(key, "value", 1893456000000LL);

    ASSERT_EQ(objectGetExpire(entry), 1893456000000LL);
    ASSERT_NE(objectGetKey(entry), nullptr);
    ASSERT_EQ(entry->hasexpire, 1);

    ASSERT_EQ(putAndWait(0, key, entry, nullptr), STORAGE_OK);

    robj *out = nullptr;
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal(out), "value"), 0);
    /* The expire is not part of the record. The keyspace holds it. */
    EXPECT_EQ(objectGetExpire(out), EXPIRY_NONE);
    decrRefCount(out);

    sdsfree(key);
    decrRefCount(entry);
}

/* A value too large to embed keeps its value in a separate allocation, so the
 * entry takes the val_ptr branch rather than the embedded one. */
TEST_F(StorageMockTest, UnembeddedValueEntryRoundTrips) {
    openEngine();
    sds key = sdsnew("big");
    std::string big(200, 'x');
    dbEntry *entry = makeEntry(key, big.c_str());
    ASSERT_EQ(entry->hasembval, 0);

    ASSERT_EQ(putAndWait(0, key, entry, nullptr), STORAGE_OK);

    robj *out = nullptr;
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal(out), big.c_str()), 0);
    decrRefCount(out);

    sdsfree(key);
    decrRefCount(entry);
}

/* An integer value is OBJ_ENCODING_INT, not an sds. It must round-trip through
 * the real serializer as its numeric value. */
TEST_F(StorageMockTest, IntEncodedValueRoundTrips) {
    openEngine();
    sds key = sdsnew("counter");
    dbEntry *entry = makeIntEntry(key, 1234567);
    ASSERT_EQ(objectGetEncoding(entry), OBJ_ENCODING_INT);

    ASSERT_EQ(putAndWait(0, key, entry, nullptr), STORAGE_OK);

    robj *out = nullptr;
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    long long v = 0;
    ASSERT_EQ(getLongLongFromObject(out, &v), C_OK);
    EXPECT_EQ(v, 1234567);
    decrRefCount(out);

    sdsfree(key);
    decrRefCount(entry);
}

/* The same key in two databases is two records. */
TEST_F(StorageMockTest, KeysAreScopedPerDatabase) {
    openEngine();
    sds key = sdsnew("same");
    dbEntry *e0 = makeEntry(key, "in-db-0");
    dbEntry *e2 = makeEntry(key, "in-db-2");

    ASSERT_EQ(putAndWait(0, key, e0, nullptr), STORAGE_OK);
    ASSERT_EQ(putAndWait(2, key, e2, nullptr), STORAGE_OK);

    robj *out = nullptr;
    ASSERT_EQ(getAndWait(2, key, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal(out), "in-db-2"), 0);
    decrRefCount(out);

    ASSERT_EQ(delAndWait(2, key), STORAGE_OK);
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_OK);
    decrRefCount(out);

    sdsfree(key);
    decrRefCount(e0);
    decrRefCount(e2);
}

TEST_F(StorageMockTest, RejectsOutOfRangeDatabase) {
    openEngine();
    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");
    EXPECT_EQ(putAndWait(cfg.num_databases, key, entry, nullptr), STORAGE_ERR_REJECTED);
    sdsfree(key);
    decrRefCount(entry);
}

TEST_F(StorageMockTest, OverwriteReplacesRatherThanAccumulates) {
    openEngine();
    sds key = sdsnew("k");
    dbEntry *first = makeEntry(key, "aaaa");
    dbEntry *second = makeEntry(key, "bb");

    ASSERT_EQ(putAndWait(0, key, first, nullptr), STORAGE_OK);
    ASSERT_EQ(putAndWait(0, key, second, nullptr), STORAGE_OK);

    StatMap st = readStats();
    EXPECT_EQ(st["total_num_items"], 1u);
    EXPECT_EQ(st["allocated_db_size_bytes"], serializedLen("bb"));

    robj *out = nullptr;
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal(out), "bb"), 0);
    decrRefCount(out);

    sdsfree(key);
    decrRefCount(first);
    decrRefCount(second);
}

TEST_F(StorageMockTest, ReportsFullWhenOverCapacity) {
    /* Two serialized five-byte values must not both fit. */
    cfg.capacity_bytes = serializedLen("12345") + 1;
    openEngine();
    sds k1 = sdsnew("k1");
    sds k2 = sdsnew("k2");
    dbEntry *e1 = makeEntry(k1, "12345");
    dbEntry *e2 = makeEntry(k2, "12345");

    ASSERT_EQ(putAndWait(0, k1, e1, nullptr), STORAGE_OK);
    EXPECT_EQ(putAndWait(0, k2, e2, nullptr), STORAGE_ERR_FULL);

    /* The rejected write left nothing behind. */
    StatMap st = readStats();
    EXPECT_EQ(st["total_num_items"], 1u);
    EXPECT_EQ(st["allocated_db_size_bytes"], serializedLen("12345"));

    sdsfree(k1);
    sdsfree(k2);
    decrRefCount(e1);
    decrRefCount(e2);
}

/* An async request is accepted but not executed at submit. Polling executes
 * the queued work and delivers its completion. */
TEST_F(StorageMockTest, AsyncPutIsNotExecutedAtSubmit) {
    openEngine();
    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");
    int token = 42;

    ASSERT_EQ(engine->put_async(0, key, sdslen(key), entry, &token), STORAGE_OK);
    EXPECT_EQ(readStats()["total_num_items"], 0u);

    /* One poll both executes the queued put and delivers its completion. */
    storageCompletion comps[4] = {};
    ASSERT_EQ(engine->poll_completions(comps, 4), 1);
    EXPECT_EQ(comps[0].request_ctx, &token);
    EXPECT_EQ(comps[0].op_type, STORAGE_OP_PUT);
    EXPECT_EQ(comps[0].status, STORAGE_OK);
    EXPECT_EQ(comps[0].stored_bytes, serializedLen("v"));

    EXPECT_EQ(readStats()["total_num_items"], 1u);

    robj *out = nullptr;
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_OK);
    decrRefCount(out);

    sdsfree(key);
    decrRefCount(entry);
}

TEST_F(StorageMockTest, AsyncGetDeliversTheValueOnTheCompletion) {
    openEngine();
    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "payload");
    ASSERT_EQ(putAndWait(0, key, entry, nullptr), STORAGE_OK);

    int token = 7;
    ASSERT_EQ(engine->get_async(0, key, sdslen(key), &token), STORAGE_OK);

    ASSERT_EQ(storageMockRunIo(2), 1);
    storageCompletion comps[2] = {};
    ASSERT_EQ(engine->poll_completions(comps, 2), 1);
    EXPECT_EQ(comps[0].op_type, STORAGE_OP_GET);
    EXPECT_EQ(comps[0].status, STORAGE_OK);
    ASSERT_NE(comps[0].value, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal((robj *)comps[0].value), "payload"), 0);
    decrRefCount((robj *)comps[0].value);

    sdsfree(key);
    decrRefCount(entry);
}

TEST_F(StorageMockTest, AsyncMissReportsNotFoundAndNoValue) {
    openEngine();
    sds key = sdsnew("absent");
    int token = 0;
    ASSERT_EQ(engine->get_async(0, key, sdslen(key), &token), STORAGE_OK);

    ASSERT_EQ(storageMockRunIo(1), 1);
    storageCompletion comps[1] = {};
    ASSERT_EQ(engine->poll_completions(comps, 1), 1);
    EXPECT_EQ(comps[0].status, STORAGE_NOT_FOUND);
    EXPECT_EQ(comps[0].value, nullptr);

    sdsfree(key);
}

TEST_F(StorageMockTest, CompletionsArriveInSubmissionOrder) {
    openEngine();
    sds k1 = sdsnew("k1");
    sds k2 = sdsnew("k2");
    dbEntry *e1 = makeEntry(k1, "v");
    dbEntry *e2 = makeEntry(k2, "v");
    int t1 = 1, t2 = 2;

    ASSERT_EQ(engine->put_async(0, k1, sdslen(k1), e1, &t1), STORAGE_OK);
    ASSERT_EQ(engine->put_async(0, k2, sdslen(k2), e2, &t2), STORAGE_OK);

    /* One IO tick executes both queued requests in submission order. */
    ASSERT_EQ(storageMockRunIo(8), 2);
    storageCompletion comps[8] = {};
    ASSERT_EQ(engine->poll_completions(comps, 8), 2);
    EXPECT_EQ(comps[0].request_ctx, &t1);
    EXPECT_EQ(comps[1].request_ctx, &t2);

    sdsfree(k1);
    sdsfree(k2);
    decrRefCount(e1);
    decrRefCount(e2);
}

/* A full request queue must return WOULDBLOCK so the caller can back off and
 * retry. Running the IO thread and draining the completions makes room. */
TEST_F(StorageMockTest, ReportsWouldBlockWhenTheQueueIsFull) {
    openEngine();
    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");

    int token = 0;
    int accepted = 0;
    for (int i = 0; i < 1000; i++) {
        if (engine->get_async(0, key, sdslen(key), &token) != STORAGE_OK) break;
        accepted++;
    }
    ASSERT_GT(accepted, 0);
    EXPECT_EQ(engine->get_async(0, key, sdslen(key), &token), STORAGE_WOULDBLOCK);

    /* Execute the queued requests, then drain the completions. Both steps are
     * needed to free the request ring. */
    ASSERT_GT(storageMockRunIo(1000), 0);
    storageCompletion comps[256] = {};
    int drained = engine->poll_completions(comps, 256);
    ASSERT_GT(drained, 0);
    for (int i = 0; i < drained; i++)
        if (comps[i].value) decrRefCount((robj *)comps[i].value);
    EXPECT_EQ(engine->put_async(0, key, sdslen(key), entry, &token), STORAGE_OK);

    /* The put borrows the entry until its completion is reported, so wait for
     * that before dropping the reference. */
    storageCompletion put_comp = {};
    ASSERT_EQ(waitForCompletedRequest(&put_comp), 1);
    EXPECT_EQ(put_comp.status, STORAGE_OK);

    sdsfree(key);
    decrRefCount(entry);
}

TEST_F(StorageMockTest, PollingAnIdleEngineReturnsNothing) {
    openEngine();
    storageCompletion comps[4] = {};
    EXPECT_EQ(engine->poll_completions(comps, 4), 0);
}

/* The key is only lent for the submit call, so the storage engine has to copy it.
 * Overwriting the caller's buffer afterwards must not move the record. */
TEST_F(StorageMockTest, TheKeyIsOnlyLentForTheCall) {
    openEngine();
    sds key = sdsnew("k1");
    dbEntry *entry = makeEntry(key, "hello");

    ASSERT_EQ(engine->put_async(0, key, sdslen(key), entry, nullptr), STORAGE_OK);
    memset(key, 'X', sdslen(key));
    ASSERT_EQ(storageMockRunIo(1), 1);
    storageCompletion comp = {};
    ASSERT_EQ(engine->poll_completions(&comp, 1), 1);
    ASSERT_EQ(comp.status, STORAGE_OK);

    sds original = sdsnew("k1");
    robj *out = nullptr;
    ASSERT_EQ(getAndWait(0, original, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal(out), "hello"), 0);
    decrRefCount(out);

    sdsfree(original);
    sdsfree(key);
    decrRefCount(entry);
}

/* A close after everything was polled leaves no record behind on reopen. */
TEST_F(StorageMockTest, ClosingAfterPollingKeepsNothing) {
    openEngine();
    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");

    ASSERT_EQ(putAndWait(0, key, entry, nullptr), STORAGE_OK);
    ASSERT_EQ(putAndWait(0, key, entry, nullptr), STORAGE_OK);
    ASSERT_EQ(engine->close(), STORAGE_OK);

    openEngine();
    StatMap st = readStats();
    EXPECT_EQ(st["total_num_items"], 0u);
    EXPECT_EQ(st["allocated_db_size_bytes"], 0u);

    robj *out = nullptr;
    EXPECT_EQ(getAndWait(0, key, &out), STORAGE_NOT_FOUND);

    sdsfree(key);
    decrRefCount(entry);
}

/* Close is refused while a request is unpolled and the storage engine stays
 * open. Once the request is polled, close goes through. */
TEST_F(StorageMockTest, CloseIsRefusedWhileARequestIsUnpolled) {
    openEngine();
    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");

    ASSERT_EQ(engine->put_async(0, key, sdslen(key), entry, nullptr), STORAGE_OK);
    EXPECT_EQ(engine->close(), STORAGE_WOULDBLOCK);

    storageCompletion comp = {};
    ASSERT_EQ(waitForCompletedRequest(&comp), 1);
    EXPECT_EQ(comp.status, STORAGE_OK);
    EXPECT_EQ(engine->close(), STORAGE_OK);

    sdsfree(key);
    decrRefCount(entry);
}

/* An IO tick executes only up to its budget. The rest stay queued for a later
 * tick. */
TEST_F(StorageMockTest, IoTickExecutesOnlyUpToItsBudget) {
    openEngine();
    sds k1 = sdsnew("k1");
    sds k2 = sdsnew("k2");
    sds k3 = sdsnew("k3");
    dbEntry *e1 = makeEntry(k1, "v");
    dbEntry *e2 = makeEntry(k2, "v");
    dbEntry *e3 = makeEntry(k3, "v");
    int t1 = 1, t2 = 2, t3 = 3;

    ASSERT_EQ(engine->put_async(0, k1, sdslen(k1), e1, &t1), STORAGE_OK);
    ASSERT_EQ(engine->put_async(0, k2, sdslen(k2), e2, &t2), STORAGE_OK);
    ASSERT_EQ(engine->put_async(0, k3, sdslen(k3), e3, &t3), STORAGE_OK);

    /* A tick with a budget of two executes two. The third stays queued. */
    ASSERT_EQ(storageMockRunIo(2), 2);

    /* A second tick executes the remainder. */
    ASSERT_EQ(storageMockRunIo(8), 1);

    /* All three completions drain in submission order. */
    storageCompletion comps[8] = {};
    ASSERT_EQ(engine->poll_completions(comps, 8), 3);
    EXPECT_EQ(comps[0].request_ctx, &t1);
    EXPECT_EQ(comps[1].request_ctx, &t2);
    EXPECT_EQ(comps[2].request_ctx, &t3);

    EXPECT_EQ(readStats()["total_num_items"], 3u);

    sdsfree(k1);
    sdsfree(k2);
    sdsfree(k3);
    decrRefCount(e1);
    decrRefCount(e2);
    decrRefCount(e3);
}

/* ---------------------------------------------------------------------------
 * Engine serialization
 *
 * Values round-trip through the DUMP wire format. These check more than plain
 * strings survive it. */

class ExtStorageSerializerTest : public ::testing::Test {
  protected:
    MockValkey mock;
    RealValkey real;

    void SetUp() override {
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
        /* rdbReportError logs through serverLog, which reads server.logfile. A
         * NULL here crashes when a malformed payload is rejected. */
        server.logfile = zstrdup("");
        /* createDumpPayload and verifyDumpPayload both use the CRC64 table, so
         * it has to be initialized or a valid payload fails its own checksum. */
        crc64_init();
        /* Compression off. These tests check the round trip, not compactness. */
        server.rdb_compression = 0;
    }

    void TearDown() override {
        zfree(server.logfile);
        server.logfile = NULL;
    }

    /* A keyspace entry */
    dbEntry *makeEntry(const char *key, const char *val, long long expire = EXPIRY_NONE) {
        sds k = sdsnew(key);
        robj *o = createStringObject(val, strlen(val));
        dbEntry *e = objectSetKeyAndExpire(o, k, expire);
        sdsfree(k);
        return e;
    }
};

TEST_F(ExtStorageSerializerTest, StringValueSurvivesTheRoundTrip) {
    const storageSerializer *s = extStorageSerializer();
    dbEntry *entry = makeEntry("mykey", "some value");

    char *bytes = nullptr;
    int len = s->serialize("mykey", 5, entry, &bytes);
    ASSERT_GT(len, 0);
    ASSERT_NE(bytes, nullptr);

    robj *back = (robj *)s->deserialize("mykey", 5, bytes, len);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ((int)back->type, OBJ_STRING);
    EXPECT_EQ(strcmp((const char *)objectGetVal(back), "some value"), 0);
    /* The record holds the value only, so what comes back has no key. */
    EXPECT_EQ(objectGetKey(back), nullptr);

    s->free_serialized(bytes);
    decrRefCount(entry);
    decrRefCount(back);
}

/* The key is the address of the record, not part of it. The same value under two
 * different keys serializes to identical bytes, and either key deserializes
 * either record. */
TEST_F(ExtStorageSerializerTest, TheKeyIsNotPartOfTheRecord) {
    const storageSerializer *s = extStorageSerializer();
    dbEntry *e1 = makeEntry("short", "same value");
    dbEntry *e2 = makeEntry("a-much-longer-key", "same value");

    char *a = nullptr;
    char *b = nullptr;
    int alen = s->serialize("short", 5, e1, &a);
    int blen = s->serialize("a-much-longer-key", 17, e2, &b);
    ASSERT_GT(alen, 0);
    ASSERT_EQ(alen, blen);
    EXPECT_EQ(memcmp(a, b, (size_t)alen), 0);

    robj *back = (robj *)s->deserialize("unrelated", 9, a, alen);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal(back), "same value"), 0);

    s->free_serialized(a);
    s->free_serialized(b);
    decrRefCount(e1);
    decrRefCount(e2);
    decrRefCount(back);
}

TEST_F(ExtStorageSerializerTest, AnExpireIsNotPartOfTheRecord) {
    const storageSerializer *s = extStorageSerializer();
    dbEntry *with = makeEntry("k", "value", 1893456000000LL);
    dbEntry *without = makeEntry("k", "value");

    char *a = nullptr;
    char *b = nullptr;
    int alen = s->serialize("k", 1, with, &a);
    int blen = s->serialize("k", 1, without, &b);
    ASSERT_GT(alen, 0);
    ASSERT_EQ(alen, blen);
    EXPECT_EQ(memcmp(a, b, (size_t)alen), 0);

    s->free_serialized(a);
    s->free_serialized(b);
    decrRefCount(with);
    decrRefCount(without);
}

TEST_F(ExtStorageSerializerTest, RejectsAMalformedBuffer) {
    const storageSerializer *s = extStorageSerializer();
    EXPECT_EQ(s->deserialize("k", 1, "not a dump payload", 18), nullptr);
    EXPECT_EQ(s->deserialize("k", 1, "", -1), nullptr);

    // A real payload with its trailing version and CRC chopped off must be
    // rejected, not loaded. This is the case that matters: the bytes parse as a
    // valid type but fail verification.
    dbEntry *entry = makeEntry("k", "some value");
    char *bytes = nullptr;
    int len = s->serialize("k", 1, entry, &bytes);
    ASSERT_GT(len, 10);
    EXPECT_EQ(s->deserialize("k", 1, bytes, len - 4), nullptr);
    s->free_serialized(bytes);
    decrRefCount(entry);
}

/* ---------------------------------------------------------------------------
 * Module data type context
 * ---------------------------------------------------------------------------*/

static std::string observed_key;
static int observed_dbid = 0;
static int observed_saves = 0;

/* The module API entry points are defined in module.c but not declared in a
 * header, since a module reaches them through the function pointer table. */
extern "C" {
const ValkeyModuleString *VM_GetKeyNameFromIO(ValkeyModuleIO *io);
int VM_GetDbIdFromIO(ValkeyModuleIO *io);
void VM_SaveUnsigned(ValkeyModuleIO *io, uint64_t value);
}

static void recordIoContextOnSave(ValkeyModuleIO *io, void *value) {
    UNUSED(value);
    const ValkeyModuleString *k = VM_GetKeyNameFromIO(io);
    observed_key = k ? std::string((const char *)objectGetVal((robj *)k), sdslen((sds)objectGetVal((robj *)k)))
                     : std::string("<null>");
    observed_dbid = VM_GetDbIdFromIO(io);
    observed_saves++;
    /* A payload is required, else the record has no module content. */
    VM_SaveUnsigned(io, 1);
}

static void freeNothingOnRelease(void *value) {
    UNUSED(value);
}

TEST_F(ExtStorageSerializerTest, AModuleTypeSeesTheKeyAndNoDbId) {
    const storageSerializer *s = extStorageSerializer();

    moduleType mt = {};
    mt.id = 1;
    mt.rdb_save = recordIoContextOnSave;
    mt.free = freeNothingOnRelease;
    strcpy(mt.name, "testtype1");

    moduleValue *mv = (moduleValue *)zmalloc(sizeof(*mv));
    mv->type = &mt;
    mv->value = nullptr;

    robj *o = createObject(OBJ_MODULE, mv);
    sds key = sdsnew("mykey");
    dbEntry *entry = objectSetKeyAndExpire(o, key, EXPIRY_NONE);

    observed_key.clear();
    observed_dbid = 0;
    observed_saves = 0;

    char *bytes = nullptr;
    int len = s->serialize(key, sdslen(key), entry, &bytes);
    ASSERT_GT(len, 0);
    ASSERT_EQ(observed_saves, 1);
    EXPECT_EQ(observed_key, "mykey");
    EXPECT_EQ(observed_dbid, -1);

    s->free_serialized(bytes);
    sdsfree(key);
    decrRefCount(entry);
}

/* ---------------------------------------------------------------------------
 * Object header storage state
 * ---------------------------------------------------------------------------*/

#define NUM_TEST_OBJECTS 12

static int createOneOfEachObject(robj **objs) {
    int n = 0;
    objs[n++] = createRawStringObject("hello", 5);
    objs[n++] = createStringObject("hello", 5);
    objs[n++] = createStringObjectFromLongLong(OBJ_SHARED_INTEGERS + 1);
    objs[n++] = createQuicklistObject(0, 0);
    objs[n++] = createListListpackObject();
    objs[n++] = createSetObject();
    objs[n++] = createIntsetObject();
    objs[n++] = createSetListpackObject();
    objs[n++] = createHashObject();
    objs[n++] = createZsetObject();
    objs[n++] = createZsetListpackObject();
    objs[n++] = createStreamObject();
    assert(n == NUM_TEST_OBJECTS);
    return n;
}

/* Adds one element to a collection, since the keyspace holds no empty ones. */
static void addOneElement(robj *o) {
    sds one = sdsnew("1");
    robj *ele = createStringObject("1", 1);
    int out_flags = 0;
    switch (o->type) {
    case OBJ_LIST: listTypePush(o, ele, LIST_TAIL); break;
    case OBJ_SET: setTypeAdd(o, one); break;
    case OBJ_HASH: hashTypeSet(o, one, one, EXPIRY_NONE, HASH_SET_COPY, NULL); break;
    case OBJ_ZSET: zsetAdd(o, 1, one, ZADD_IN_NONE, &out_flags, NULL); break;
    }
    decrRefCount(ele);
    sdsfree(one);
}

TEST(StorageStateTest, NewObjectsStartInMemory) {
    robj *objs[NUM_TEST_OBJECTS];
    int n = createOneOfEachObject(objs);
    for (int i = 0; i < n; i++) {
        EXPECT_EQ(getStorageState(objs[i]), STORAGE_STATE_ONLY_MEMORY) << "type " << objs[i]->type << " encoding "
                                                                       << strEncoding(objs[i]->encoding);
        decrRefCount(objs[i]);
    }

    robj stack;
    initStaticStringObject(stack, NULL);
    EXPECT_EQ(getStorageState(&stack), STORAGE_STATE_ONLY_MEMORY);
}

TEST(StorageStateTest, SetStorageState) {
    robj *objs[NUM_TEST_OBJECTS];
    int n = createOneOfEachObject(objs);
    for (int i = 0; i < n; i++) {
        for (int state = 0; state < STORAGE_STATE_NUM_STATES; state++) {
            setStorageState(objs[i], (storageState)state);
            EXPECT_EQ(getStorageState(objs[i]), (storageState)state)
                << "type " << objs[i]->type << " encoding " << strEncoding(objs[i]->encoding);
        }
        setStorageState(objs[i], STORAGE_STATE_ONLY_MEMORY);
        decrRefCount(objs[i]);
    }
}

/* ---------------------------------------------------------------------------
 * Spill candidate selection
 * ---------------------------------------------------------------------------*/

class SpillSelectTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        static bool pool_allocated = false;
        if (!pool_allocated) evictionPoolAlloc();
        pool_allocated = true;
    }

    void SetUp() override {
        memset(&server, 0, sizeof(valkeyServer));
        server.dbnum = 1;
        server.maxmemory_samples = 5;
        server.maxmemory_policy = MAXMEMORY_ALLKEYS_LRU;
        server.db = (serverDb **)zcalloc(sizeof(serverDb *));
        server.db[0] = (serverDb *)zcalloc(sizeof(serverDb));
        server.db[0]->keys = kvstoreCreate(&kvstoreKeysHashtableType, 0, 0);
        server.db[0]->expires = kvstoreCreate(&kvstoreExpiresHashtableType, 0, 0);
    }

    void TearDown() override {
        /* With the keys gone, one more selection empties the shared pool. */
        drainPool();
        kvstoreRelease(server.db[0]->expires);
        kvstoreRelease(server.db[0]->keys);
        zfree(server.db[0]);
        zfree(server.db);
    }

    void drainPool(void) {
        for (int i = 0; i < server.dbnum; i++) {
            kvstoreEmpty(server.db[i]->expires, NULL);
            kvstoreEmpty(server.db[i]->keys, NULL);
        }
        int dbid;
        usePolicy(MAXMEMORY_ALLKEYS_LRU);
        extStorageSpillSelectCandidate(&dbid);
    }

    void usePolicy(int policy) {
        server.maxmemory_policy = policy;
        lrulfu_updateClockAndPolicy(mstime(), (policy & MAXMEMORY_FLAG_LFU) != 0);
    }

    /* Runs performEvictions() one byte over maxmemory, so it evicts one key and
     * leaves the rest of its sample in the pool. */
    void evictOneKey(void) {
        /* Evicting a key notifies modules, touches watched keys and propagates a DEL. */
        static bool server_initialized = false;
        if (!server_initialized) {
            monotonicInit();
            createSharedObjects();
            moduleInitModulesSystem();
            server_initialized = true;
        }
        server.replicas = listCreate();
        server.db[0]->watched_keys = dictCreate(&keylistDictType);

        server.maxmemory = zmalloc_used_memory() - 1;
        EXPECT_EQ(performEvictions(), EVICT_OK);
        server.maxmemory = 0;

        dictRelease(server.db[0]->watched_keys);
        server.db[0]->watched_keys = NULL;
        listRelease(server.replicas);
        server.replicas = NULL;
    }

    bool keyExists(const char *key) {
        sds k = sdsnew(key);
        void *entry;
        bool found = kvstoreHashtableFind(server.db[0]->keys, 0, k, &entry);
        sdsfree(k);
        return found;
    }

    /* Whether spill selection would pick 'entry', the only candidate there. */
    bool isSelected(robj *entry) {
        int dbid;
        return extStorageSpillSelectCandidate(&dbid) == entry;
    }

    /* Adds a key to 'db' in 'slot', with a TTL unless expire is -1, and returns its entry. */
    robj *addKeyTo(serverDb *db, int slot, const char *key, robj *val, long long expire) {
        sds k = sdsnew(key);
        robj *entry = objectSetKeyAndExpire(val, k, expire);
        sdsfree(k);
        initObjectLRUOrLFU(entry);
        kvstoreHashtableAdd(db->keys, slot, entry);
        if (expire != -1) kvstoreHashtableAdd(db->expires, slot, entry);
        return entry;
    }

    robj *addKeyWithExpire(const char *key, robj *val, long long expire) {
        return addKeyTo(server.db[0], 0, key, val, expire);
    }

    robj *addKey(const char *key, robj *val) {
        return addKeyWithExpire(key, val, -1);
    }

    robj *rawValue(void) {
        /* Too big to embed. */
        char val[200];
        memset(val, 'v', sizeof(val));
        return createStringObject(val, sizeof(val));
    }

    robj *addRawKeyWithExpire(const char *key, long long expire) {
        robj *entry = addKeyWithExpire(key, rawValue(), expire);
        EXPECT_EQ(entry->encoding, OBJ_ENCODING_RAW);
        EXPECT_FALSE(entry->hasembval);
        return entry;
    }

    robj *addRawKey(const char *key) {
        return addRawKeyWithExpire(key, -1);
    }
};

/* Only embedded and INT strings have no separate value to free. */
TEST_F(SpillSelectTest, EveryTypeIsACandidateExceptEmbeddedAndIntStrings) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    robj *objs[NUM_TEST_OBJECTS];
    int n = createOneOfEachObject(objs);
    for (int i = 0; i < n; i++) {
        int type = objs[i]->type;
        int encoding = objs[i]->encoding;
        bool expected = encoding != OBJ_ENCODING_EMBSTR && encoding != OBJ_ENCODING_INT;
        addOneElement(objs[i]);
        robj *entry = addKey("key", objs[i]);

        int dbid = -1;
        EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), expected ? entry : nullptr)
            << "type " << type << " encoding " << strEncoding(encoding);
        kvstoreEmpty(server.db[0]->keys, NULL);
    }
}

TEST_F(SpillSelectTest, PicksTheLeastRecentlyUsedKeyUnderAllkeysLru) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    objectSetLRUOrLFU(addRawKey("recent"), -1, 10);
    robj *oldest = addRawKey("oldest");
    objectSetLRUOrLFU(oldest, -1, 1000);
    objectSetLRUOrLFU(addRawKey("middle"), -1, 100);

    int dbid = -1;
    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), oldest);
    EXPECT_EQ(dbid, 0);
}

TEST_F(SpillSelectTest, PicksTheLeastFrequentlyUsedKeyUnderAllkeysLfu) {
    usePolicy(MAXMEMORY_ALLKEYS_LFU);
    objectSetLRUOrLFU(addRawKey("hot"), 200, -1);
    robj *coldest = addRawKey("coldest");
    objectSetLRUOrLFU(coldest, 1, -1);
    objectSetLRUOrLFU(addRawKey("warm"), 50, -1);

    int dbid = -1;
    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), coldest);
}

/* Colder keys that can't be spilled are passed over. */
TEST_F(SpillSelectTest, SkipsKeysThatCannotBeSpilled) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    robj *embedded = addKey("embedded", createStringObject("short", 5));
    ASSERT_TRUE(embedded->hasembval);
    objectSetLRUOrLFU(embedded, -1, 5000);

    robj *int_encoded = addKey("int", createStringObjectFromLongLong(OBJ_SHARED_INTEGERS + 1));
    ASSERT_EQ(int_encoded->encoding, OBJ_ENCODING_INT);
    objectSetLRUOrLFU(int_encoded, -1, 4000);

    robj *in_flight = addRawKey("in_flight");
    setStorageState(in_flight, STORAGE_STATE_COPYING_TO_FLASH);
    objectSetLRUOrLFU(in_flight, -1, 3000);

    robj *referenced = addRawKey("referenced");
    incrRefCount(referenced);
    objectSetLRUOrLFU(referenced, -1, 2000);

    robj *candidate = addRawKey("candidate");
    objectSetLRUOrLFU(candidate, -1, 10);

    int dbid = -1;
    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), candidate);
    decrRefCount(referenced);
}

/* Module keys aren't spillable yet (#81). */
TEST_F(SpillSelectTest, SkipsModuleKeys) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    static moduleType mt = {};
    mt.free = freeNothingOnRelease;
    moduleValue *mv = (moduleValue *)zmalloc(sizeof(*mv));
    mv->type = &mt;
    mv->value = nullptr;
    robj *module_key = addKey("module", createObject(OBJ_MODULE, mv));
    ASSERT_FALSE(module_key->hasembval);
    objectSetLRUOrLFU(module_key, -1, 5000);

    robj *candidate = addRawKey("candidate");
    objectSetLRUOrLFU(candidate, -1, 10);

    int dbid = -1;
    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), candidate);
}

/* An expired key that expiry hasn't deleted yet would be written to flash for
 * nothing, however cold it is. */
TEST_F(SpillSelectTest, SkipsExpiredKeys) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    server.cmd_time_snapshot = mstime();
    objectSetLRUOrLFU(addRawKeyWithExpire("expired", server.cmd_time_snapshot - 1000), -1, 5000);
    robj *live = addRawKeyWithExpire("live", server.cmd_time_snapshot + 3600 * 1000);
    objectSetLRUOrLFU(live, -1, 10);

    int dbid = -1;
    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), live);
}

/* Field expiry reads and changes the hash's value, so it has to stay in DRAM. */
TEST_F(SpillSelectTest, SkipsHashesWithFieldExpirations) {
    robj *hash = createHashObject();
    hashTypeSet(hash, sdsnew("field"), sdsnew("value"), EXPIRY_NONE, HASH_SET_TAKE_FIELD | HASH_SET_TAKE_VALUE, NULL);
    robj *entry = addKey("hash", hash);
    EXPECT_TRUE(isSelected(entry));
    hashTypeSet(entry, sdsnew("ttl"), sdsnew("value"), mstime() + 100000, HASH_SET_TAKE_FIELD | HASH_SET_TAKE_VALUE,
                NULL);
    EXPECT_FALSE(isSelected(entry));
}

/* Non-candidates must not crowd the candidate out of the pool. */
TEST_F(SpillSelectTest, KeysThatCannotBeSpilledDoNotFillThePool) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    server.maxmemory_samples = 64; /* sample every key in one pass */
    char name[32];
    for (int i = 0; i < 20; i++) {
        snprintf(name, sizeof(name), "embedded:%d", i);
        objectSetLRUOrLFU(addKey(name, createStringObject("short", 5)), -1, 1000 + i);
    }
    robj *candidate = addRawKey("candidate");
    objectSetLRUOrLFU(candidate, -1, 10);

    int dbid = -1;
    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), candidate);
}

/* Deleted keys left in the pool must not hide a live one. */
TEST_F(SpillSelectTest, DeletedPooledKeysDoNotHideACandidate) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    server.maxmemory_samples = 64; /* sample every key in one pass */
    char name[32];
    for (int i = 0; i < 20; i++) {
        snprintf(name, sizeof(name), "cold:%d", i);
        objectSetLRUOrLFU(addRawKey(name), -1, 1000 + i);
    }
    int dbid = -1;
    ASSERT_NE(extStorageSpillSelectCandidate(&dbid), nullptr);

    /* Leave the pool holding only deleted keys. */
    for (int i = 0; i < 20; i++) {
        snprintf(name, sizeof(name), "cold:%d", i);
        sds key = sdsnew(name);
        kvstoreHashtableDelete(server.db[0]->keys, 0, key);
        sdsfree(key);
    }
    robj *candidate = addRawKey("candidate");
    objectSetLRUOrLFU(candidate, -1, 10);

    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), candidate);
}

/* A pooled key that has since been overwritten with a short value is skipped. */
TEST_F(SpillSelectTest, SkipsPooledKeysOverwrittenWithEmbeddedValues) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    server.maxmemory_samples = 64; /* sample every key in one pass */
    robj *coldest = addRawKey("coldest");
    objectSetLRUOrLFU(coldest, -1, 3000);
    objectSetLRUOrLFU(addRawKey("overwritten"), -1, 2000);
    robj *candidate = addRawKey("candidate");
    objectSetLRUOrLFU(candidate, -1, 10);

    int dbid = -1;
    ASSERT_EQ(extStorageSpillSelectCandidate(&dbid), coldest);
    setStorageState(coldest, STORAGE_STATE_COPYING_TO_FLASH); /* submitted */

    /* As dbSetValue() does for a short value: a new embedded object under the
     * same name, keeping the old LRU. */
    sds key = sdsnew("overwritten");
    kvstoreHashtableDelete(server.db[0]->keys, 0, key);
    sdsfree(key);
    robj *embedded = addKey("overwritten", createStringObject("short", 5));
    ASSERT_TRUE(embedded->hasembval);
    objectSetLRUOrLFU(embedded, -1, 2000);

    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), candidate);
}

TEST_F(SpillSelectTest, ReturnsNullWhenNoKeyCanBeSpilled) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    addKey("embedded", createStringObject("short", 5));
    setStorageState(addRawKey("on_flash"), STORAGE_STATE_ONLY_FLASH);

    int dbid = -1;
    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), nullptr);
}

/* Spill ranks every key by LRU or LFU whatever the maxmemory-policy: keys
 * without a TTL are picked under volatile-*, and volatile-ttl doesn't rank by
 * TTL. */
TEST_F(SpillSelectTest, PicksTheColdestKeyUnderEveryPolicy) {
    server.maxmemory_samples = 64; /* sample every key in one pass */
    int policies[] = {MAXMEMORY_ALLKEYS_LRU, MAXMEMORY_ALLKEYS_LFU, MAXMEMORY_VOLATILE_LRU,
                      MAXMEMORY_VOLATILE_LFU, MAXMEMORY_VOLATILE_TTL, MAXMEMORY_VOLATILE_RANDOM,
                      MAXMEMORY_ALLKEYS_RANDOM, MAXMEMORY_NO_EVICTION};
    for (int i = 0; i < 8; i++) {
        usePolicy(policies[i]);
        bool lfu = policies[i] & MAXMEMORY_FLAG_LFU;
        robj *coldest = addRawKey("coldest");
        robj *warm = addRawKeyWithExpire("warm", mstime() + 3600 * 1000);
        robj *expires_soonest = addRawKeyWithExpire("expires_soonest", mstime() + 1000);
        objectSetLRUOrLFU(coldest, lfu ? 1 : -1, lfu ? -1 : 5000);
        objectSetLRUOrLFU(warm, lfu ? 50 : -1, lfu ? -1 : 100);
        objectSetLRUOrLFU(expires_soonest, lfu ? 200 : -1, lfu ? -1 : 10);

        int dbid = -1;
        EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), coldest) << "policy " << policies[i];

        drainPool();
    }
}

/* Under volatile-ttl eviction leaves entries scored by expiry in the pool.
 * Those outrank every idleness score, so spill must not pick from them, also
 * after maxmemory-policy has been changed away from volatile-ttl. */
TEST_F(SpillSelectTest, IgnoresTtlScoresEvictionLeftInThePool) {
    server.maxmemory_samples = 64; /* sample every key in one pass */
    int policies[] = {MAXMEMORY_VOLATILE_TTL, MAXMEMORY_ALLKEYS_LRU, MAXMEMORY_NO_EVICTION};
    for (int i = 0; i < 3; i++) {
        usePolicy(MAXMEMORY_VOLATILE_TTL);
        long long now = mstime();
        robj *coldest = addRawKey("coldest");
        objectSetLRUOrLFU(coldest, -1, 5000);
        objectSetLRUOrLFU(addRawKeyWithExpire("expires_soon", now + 1000), -1, 10);
        objectSetLRUOrLFU(addRawKeyWithExpire("expires_later", now + 2000), -1, 10);

        /* Evicts expires_soon and leaves expires_later in the pool. */
        evictOneKey();
        ASSERT_FALSE(keyExists("expires_soon"));

        usePolicy(policies[i]);
        int dbid = -1;
        EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), coldest) << "policy " << policies[i];
        drainPool();
    }
}

/* A key that loses its TTL after eviction pooled it is no longer evictable
 * under a volatile policy, so eviction must skip it and pick another key. */
TEST_F(SpillSelectTest, EvictionSkipsPooledKeysThatLostTheirTtl) {
    usePolicy(MAXMEMORY_VOLATILE_LRU);
    server.maxmemory_samples = 64; /* sample every key in one pass */
    long long expire = mstime() + 3600 * 1000;
    objectSetLRUOrLFU(addRawKeyWithExpire("coldest", expire), -1, 5000);
    robj *persisted = addRawKeyWithExpire("persisted", expire);
    objectSetLRUOrLFU(persisted, -1, 4000);
    objectSetLRUOrLFU(addRawKeyWithExpire("recent", expire), -1, 10);

    /* Evicts coldest and leaves persisted and recent in the pool. */
    evictOneKey();
    ASSERT_FALSE(keyExists("coldest"));

    /* As PERSIST does. */
    void *popped;
    ASSERT_TRUE(kvstoreHashtablePop(server.db[0]->expires, 0, objectGetKey(persisted), &popped));
    ASSERT_EQ(objectSetExpire(persisted, -1), persisted);

    evictOneKey();
    EXPECT_TRUE(keyExists("persisted"));
    EXPECT_FALSE(keyExists("recent"));
}

TEST_F(SpillSelectTest, ReportsTheDbOfTheChosenKey) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    server.maxmemory_samples = 64; /* sample every key in one pass */
    server.dbnum = 3;
    server.db = (serverDb **)zrealloc(server.db, sizeof(serverDb *) * 3);
    for (int i = 1; i < 3; i++) {
        server.db[i] = (serverDb *)zcalloc(sizeof(serverDb));
        server.db[i]->id = i;
        server.db[i]->keys = kvstoreCreate(&kvstoreKeysHashtableType, 0, 0);
        server.db[i]->expires = kvstoreCreate(&kvstoreExpiresHashtableType, 0, 0);
    }
    objectSetLRUOrLFU(addKeyTo(server.db[0], 0, "recent", rawValue(), -1), -1, 100);
    objectSetLRUOrLFU(addKeyTo(server.db[1], 0, "warm", rawValue(), -1), -1, 200);
    robj *coldest = addKeyTo(server.db[2], 0, "coldest", rawValue(), -1);
    objectSetLRUOrLFU(coldest, -1, 5000);

    int dbid = -1;
    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), coldest);
    EXPECT_EQ(dbid, 2);

    drainPool();
    for (int i = 1; i < 3; i++) {
        kvstoreRelease(server.db[i]->expires);
        kvstoreRelease(server.db[i]->keys);
        zfree(server.db[i]);
    }
    server.dbnum = 1;
}

/* Key names longer than the pool's cached buffer get their own copy, which the
 * pool must free when it pops the entry. */
TEST_F(SpillSelectTest, FreesPoolCopiesOfLongKeyNames) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    server.maxmemory_samples = 64; /* sample every key in one pass */
    char long_name[300];
    memset(long_name, 'k', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    robj *long_key = addRawKey(long_name);
    objectSetLRUOrLFU(long_key, -1, 100);
    robj *coldest = addRawKey("coldest");
    objectSetLRUOrLFU(coldest, -1, 5000);

    size_t used = zmalloc_used_memory();
    int dbid = -1;
    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), coldest);

    /* With neither key spillable, the next selection pops the long name
     * without pooling it again. */
    setStorageState(coldest, STORAGE_STATE_COPYING_TO_FLASH);
    setStorageState(long_key, STORAGE_STATE_COPYING_TO_FLASH);
    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), nullptr);
    EXPECT_EQ(zmalloc_used_memory(), used);
}

/* Keys sampled by an earlier selection stay in the pool, so a later selection
 * still finds them when its own sample misses them. */
TEST_F(SpillSelectTest, KeepsCandidatesFromEarlierSelections) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    server.maxmemory_samples = 64; /* sample every key in one pass */
    robj *coldest = addRawKey("coldest");
    objectSetLRUOrLFU(coldest, -1, 5000);
    robj *second = addRawKey("second");
    objectSetLRUOrLFU(second, -1, 4000);

    int dbid = -1;
    ASSERT_EQ(extStorageSpillSelectCandidate(&dbid), coldest);
    setStorageState(coldest, STORAGE_STATE_COPYING_TO_FLASH); /* submitted */

    /* One sample out of a thousand recent keys is unlikely to hit second. */
    char name[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(name, sizeof(name), "recent:%d", i);
        addRawKey(name);
    }
    server.maxmemory_samples = 1;

    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), second);
}

/* Eviction under volatile-lru and spill under allkeys-lru both score by idle
 * time. Only the scale decides whether the pool is cleared, so the keys
 * eviction left in the pool must stay there for spill to pick. */
TEST_F(SpillSelectTest, KeepsEvictionCandidatesScoredOnTheSameScale) {
    usePolicy(MAXMEMORY_VOLATILE_LRU);
    server.maxmemory_samples = 64; /* sample every key in one pass */
    long long expire = mstime() + 3600 * 1000;
    objectSetLRUOrLFU(addRawKeyWithExpire("coldest", expire), -1, 5000);
    robj *second = addRawKeyWithExpire("second", expire);
    objectSetLRUOrLFU(second, -1, 4000);
    objectSetLRUOrLFU(addRawKeyWithExpire("warm", expire), -1, 10);

    /* Evicts coldest and leaves second and warm in the pool. */
    evictOneKey();
    ASSERT_FALSE(keyExists("coldest"));

    /* One sample out of a thousand recent keys is unlikely to hit second. */
    char name[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(name, sizeof(name), "recent:%d", i);
        addRawKey(name);
    }
    server.maxmemory_samples = 1;

    int dbid = -1;
    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), second);
}

/* LRU scores are idle times and LFU scores are at most 255, so entries scored
 * under LRU would outrank every key once the policy switches to LFU. */
TEST_F(SpillSelectTest, IgnoresLruScoresAfterSwitchingToLfu) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    server.maxmemory_samples = 64; /* sample every key in one pass */
    robj *coldest = addRawKey("coldest");
    objectSetLRUOrLFU(coldest, -1, 5000);
    robj *frequent = addRawKey("frequent");
    objectSetLRUOrLFU(frequent, -1, 4000);

    int dbid = -1;
    ASSERT_EQ(extStorageSpillSelectCandidate(&dbid), coldest);
    setStorageState(coldest, STORAGE_STATE_COPYING_TO_FLASH); /* submitted */

    usePolicy(MAXMEMORY_ALLKEYS_LFU);
    objectSetLRUOrLFU(frequent, 200, -1);
    robj *rare = addRawKey("rare");
    objectSetLRUOrLFU(rare, 1, -1);

    EXPECT_EQ(extStorageSpillSelectCandidate(&dbid), rare);
}

/* In cluster mode keys live in per-slot tables. With one key per slot and a
 * sample size of two, every selection samples two different slots, and the
 * pool must look each key up in the slot it came from. */
TEST_F(SpillSelectTest, PicksKeysAcrossSlots) {
    usePolicy(MAXMEMORY_ALLKEYS_LRU);
    server.maxmemory_samples = 2;
    kvstoreRelease(server.db[0]->expires);
    kvstoreRelease(server.db[0]->keys);
    server.db[0]->keys = kvstoreCreate(&kvstoreKeysHashtableType, 14, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND);
    server.db[0]->expires = kvstoreCreate(&kvstoreExpiresHashtableType, 14, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND);

    /* Ten times the sample size, so sampling moves on to a second slot. */
    char name[32];
    for (int i = 0; i < 20; i++) {
        snprintf(name, sizeof(name), "key:%d", i);
        objectSetLRUOrLFU(addKeyTo(server.db[0], 100 + i * 800, name, rawValue(), -1), -1, 100 + i);
    }

    for (int i = 0; i < 10; i++) {
        int dbid = -1;
        EXPECT_NE(extStorageSpillSelectCandidate(&dbid), nullptr) << "selection " << i;
    }
}

/* ---------------------------------------------------------------------------
 * Spill submission
 * ---------------------------------------------------------------------------*/

class SpillSubmitTest : public SpillSelectTest {
  protected:
    void SetUp() override {
        SpillSelectTest::SetUp();
        extStorageTestResetRegistry();
        server.hz = CONFIG_DEFAULT_HZ;
        server.logfile = zstrdup("");
        crc64_init();
        server.ext_storage_enabled = 1;
        server.ext_storage_engine = zstrdup("mock");
        server.ext_storage_path = zstrdup("");
        extStorageInit();
    }

    void TearDown() override {
        storageMockRunIo(1000);
        storageCompletion done[16];
        int n;
        while ((n = storageMockEngine()->poll_completions(done, 16)) > 0) {
            for (int i = 0; i < n; i++) {
                if (done[i].op_type == STORAGE_OP_PUT) zfree(done[i].request_ctx);
            }
        }
        extStorageDeinit();
        zfree(server.ext_storage_engine);
        zfree(server.ext_storage_path);
        zfree(server.logfile);
        SpillSelectTest::TearDown();
    }
};

TEST_F(SpillSubmitTest, SubmitsTheValue) {
    robj *entry = addRawKey("key");

    ASSERT_EQ(extStorageSpillItem(0, entry), STORAGE_OK);
    EXPECT_EQ(getStorageState(entry), STORAGE_STATE_COPYING_TO_FLASH);

    /* The engine writes the borrowed value and hands back our request. */
    ASSERT_EQ(storageMockRunIo(1), 1);
    storageCompletion comp = {};
    ASSERT_EQ(storageMockEngine()->poll_completions(&comp, 1), 1);
    EXPECT_EQ(comp.op_type, STORAGE_OP_PUT);
    EXPECT_EQ(comp.status, STORAGE_OK);
    EXPECT_NE(comp.request_ctx, nullptr);
    zfree(comp.request_ctx);
}

/* Each type and encoding takes its own path through the serializer, which
 * fails the put if it can't serialize the value. */
TEST_F(SpillSubmitTest, SubmitsEveryTypeOfValue) {
    robj *objs[NUM_TEST_OBJECTS];
    int n = createOneOfEachObject(objs);
    for (int i = 0; i < n; i++) {
        int type = objs[i]->type;
        int encoding = objs[i]->encoding;
        if (encoding == OBJ_ENCODING_EMBSTR || encoding == OBJ_ENCODING_INT) {
            decrRefCount(objs[i]); /* not spill candidates */
            continue;
        }
        addOneElement(objs[i]);
        char name[32];
        snprintf(name, sizeof(name), "key:%d", i);
        robj *entry = addKey(name, objs[i]);

        ASSERT_EQ(extStorageSpillItem(0, entry), STORAGE_OK) << "type " << type << " encoding " << strEncoding(encoding);
        ASSERT_EQ(storageMockRunIo(1), 1);
        storageCompletion comp = {};
        ASSERT_EQ(storageMockEngine()->poll_completions(&comp, 1), 1);
        EXPECT_EQ(comp.status, STORAGE_OK) << "type " << type << " encoding " << strEncoding(encoding);
        zfree(comp.request_ctx);
    }
}

TEST_F(SpillSubmitTest, WouldBlockChangesNothing) {
    /* Fill the engine's queue. */
    int queued = 0;
    for (; queued < 1000; queued++) {
        char key[32];
        snprintf(key, sizeof(key), "queued%d", queued);
        if (extStorageSpillItem(0, addRawKey(key)) != STORAGE_OK) break;
    }
    ASSERT_LT(queued, 1000) << "the engine's queue never filled";

    robj *entry = addRawKey("key");

    EXPECT_EQ(extStorageSpillItem(0, entry), STORAGE_WOULDBLOCK);
    EXPECT_EQ(getStorageState(entry), STORAGE_STATE_ONLY_MEMORY);
}

TEST_F(SpillSubmitTest, ARejectedItemChangesNothing) {
    robj *entry = addRawKey("key");

    /* The engine rejects a database it wasn't opened with. */
    EXPECT_EQ(extStorageSpillItem(server.dbnum, entry), STORAGE_ERR_REJECTED);
    EXPECT_EQ(getStorageState(entry), STORAGE_STATE_ONLY_MEMORY);
}

/* ---------------------------------------------------------------------------
 * Active defrag
 * ---------------------------------------------------------------------------*/

#ifdef HAVE_DEFRAG

extern "C" int defragLaterItem(robj *ob, unsigned long *cursor, monotime endtime, int dbid);

/* A big list deferred by defrag can be spilled before defrag gets back to it.
 * Defrag must then drop it without walking the value the engine is reading. */
TEST(DefragLaterTest, SkipsAValueThatIsNotOnlyInMemory) {
    robj *list = createQuicklistObject(0, 0);
    robj *ele = createStringObject("v", 1);
    for (int i = 0; i < 10; i++) listTypePush(list, ele, LIST_TAIL);
    decrRefCount(ele);

    for (int state = STORAGE_STATE_COPYING_TO_FLASH; state < STORAGE_STATE_NUM_STATES; state++) {
        setStorageState(list, (storageState)state);
        long long scanned = server.stat_active_defrag_scanned;
        unsigned long cursor = 0;

        EXPECT_EQ(defragLaterItem(list, &cursor, (monotime)-1, 0), 0) << "state " << state;
        EXPECT_EQ(server.stat_active_defrag_scanned, scanned) << "state " << state;
    }

    setStorageState(list, STORAGE_STATE_ONLY_MEMORY);
    decrRefCount(list);
}

#endif /* HAVE_DEFRAG */

/* ---------------------------------------------------------------------------
 * Fetch completion
 * ---------------------------------------------------------------------------*/

class FetchCompletionTest : public SpillSelectTest {
  protected:
    const storageEngine *engine = storageMockEngine();

    void SetUp() override {
        SpillSelectTest::SetUp();
        extStorageTestResetRegistry();
        server.logfile = zstrdup("");
        crc64_init();
        server.ext_storage_enabled = 1;
        server.ext_storage_engine = zstrdup("mock");
        server.ext_storage_path = zstrdup("");
        extStorageInit();
    }

    void TearDown() override {
        extStorageDeinit();
        zfree(server.ext_storage_engine);
        zfree(server.ext_storage_path);
        zfree(server.logfile);
        SpillSelectTest::TearDown();
    }

    /* Stores the key's value in the engine and frees it from memory, as a spill would. */
    void spill(robj *entry) {
        sds key = objectGetKey(entry);
        ASSERT_EQ(engine->put_async(0, key, sdslen(key), entry, nullptr), STORAGE_OK);
        storageMockRunIo(1);
        storageCompletion comp;
        ASSERT_EQ(engine->poll_completions(&comp, 1), 1);
        sdsfree((sds)objectGetVal(entry));
        objectSetVal(entry, NULL);
        setStorageState(entry, STORAGE_STATE_ONLY_FLASH);
    }

    /* Submits a fetch the way fetchKeyAsync does. */
    void startFetch(robj *entry) {
        sds key = objectGetKey(entry);
        robj *ctx = createStringObject(key, sdslen(key));
        ASSERT_EQ(engine->get_async(0, key, sdslen(key), ctx), STORAGE_OK);
        setStorageState(entry, STORAGE_STATE_COPYING_TO_MEMORY);
    }
};

TEST_F(FetchCompletionTest, AFetchedValueIsBackInMemory) {
    robj *entry = addRawKey("key");
    spill(entry);
    startFetch(entry);

    externalStorage_processCompletedRequests();

    robj *expected = rawValue();
    EXPECT_EQ(getStorageState(entry), STORAGE_STATE_ONLY_MEMORY);
    EXPECT_EQ(sdscmp((sds)objectGetVal(entry), (sds)objectGetVal(expected)), 0);
    decrRefCount(expected);
}

TEST_F(FetchCompletionTest, AFailedFetchPanics) {
    startFetch(addRawKey("key")); /* never spilled, so the engine returns NOT_FOUND */
    // todo: match on the assert message instead of passing on any crash
    EXPECT_DEATH(externalStorage_processCompletedRequests(), "");
}

TEST_F(FetchCompletionTest, AFetchForADeletedKeyPanics) {
    robj *entry = addRawKey("key");
    spill(entry);
    startFetch(entry);
    kvstoreHashtableDelete(server.db[0]->keys, 0, objectGetKey(entry));
    // todo: match on the assert message instead of passing on any crash
    EXPECT_DEATH(externalStorage_processCompletedRequests(), "");
}

/* ---------------------------------------------------------------------------
 * Client blocking
 * ---------------------------------------------------------------------------*/

class BlockClientTest : public FetchCompletionTest {
  protected:
    static inline hashtable *commands = nullptr;
    static inline hashtable *orig_commands = nullptr;
    static inline ConnectionType conn_type = {0};
    client *clients[8];
    int num_clients = 0;

    static int setReadHandler(connection *conn, ConnectionCallbackFunc func) {
        conn->read_handler = func;
        return C_OK;
    }

    void SetUp() override {
        FetchCompletionTest::SetUp();
        if (!commands) {
            server.commands = hashtableCreate(&commandSetType);
            server.orig_commands = hashtableCreate(&commandSetType);
            populateCommandTable();
            commands = server.commands;
            orig_commands = server.orig_commands;
            conn_type.set_read_handler = setReadHandler;
        }
        server.commands = commands;
        server.orig_commands = orig_commands;
        server.unblocked_clients = listCreate();
    }

    void TearDown() override {
        for (int j = 0; j < num_clients; j++) {
            client *c = clients[j];
            for (int i = 0; i < c->argc; i++) decrRefCount(c->argv[i]);
            zfree(c->argv);
            freeClientBlockingState(c);
            zfree(c->conn);
            zfree(c);
        }
        listRelease(server.unblocked_clients);
        FetchCompletionTest::TearDown();
    }

    /* A client about to run 'cmdline', such as "mget a b", on db 0. */
    client *makeClient(const char *cmdline) {
        client *c = (client *)zcalloc(sizeof(client));
        sds *args = sdssplitargs(cmdline, &c->argc);
        c->argv = (robj **)zmalloc(sizeof(robj *) * c->argc);
        for (int i = 0; i < c->argc; i++) c->argv[i] = createObject(OBJ_STRING, args[i]);
        zfree(args);
        c->cmd = lookupCommandByCString((const char *)objectGetVal(c->argv[0]));
        c->db = server.db[0];
        c->conn = (connection *)zcalloc(sizeof(connection));
        c->conn->type = &conn_type;
        c->flag.pending_command = 1;
        clients[num_clients++] = c;
        return c;
    }

    robj *addFlashKey(const char *key) {
        robj *entry = addRawKey(key);
        spill(entry);
        return entry;
    }

    bool isBlocked(client *c) {
        return c->flag.blocked && c->bstate->btype == BLOCKED_INUSE;
    }

    uint64_t numFetched() {
        storageStats st;
        engine->get_stats(&st);
        return st.total_num_items_fetched_from_storage;
    }
};

TEST_F(BlockClientTest, NoKeysOnFlashDoesNotBlock) {
    addRawKey("a");
    addRawKey("b");
    client *c = makeClient("mget a b");

    EXPECT_FALSE(externalStorage_blockClientIfNeeded(c));
    EXPECT_FALSE(c->flag.blocked);
}

TEST_F(BlockClientTest, AKeyOnFlashBlocksUntilFetched) {
    robj *a = addFlashKey("a");
    client *c = makeClient("get a");

    EXPECT_TRUE(externalStorage_blockClientIfNeeded(c));
    EXPECT_TRUE(isBlocked(c));
    EXPECT_EQ(getStorageState(a), STORAGE_STATE_COPYING_TO_MEMORY);

    externalStorage_processCompletedRequests();
    EXPECT_FALSE(isBlocked(c));
    EXPECT_TRUE(c->flag.unblocked);
    EXPECT_EQ(getStorageState(a), STORAGE_STATE_ONLY_MEMORY);
}

TEST_F(BlockClientTest, MultipleKeysOnFlashBlockUntilAllAreFetched) {
    robj *a = addFlashKey("a");
    robj *b = addFlashKey("b");
    client *c = makeClient("mget a b");

    EXPECT_TRUE(externalStorage_blockClientIfNeeded(c));
    EXPECT_TRUE(isBlocked(c));

    externalStorage_processCompletedRequests();
    EXPECT_FALSE(isBlocked(c));
    EXPECT_EQ(getStorageState(a), STORAGE_STATE_ONLY_MEMORY);
    EXPECT_EQ(getStorageState(b), STORAGE_STATE_ONLY_MEMORY);
}

TEST_F(BlockClientTest, OnlyTheKeysOnFlashBlock) {
    addRawKey("a");
    addFlashKey("b");
    client *c = makeClient("mget a b");

    EXPECT_TRUE(externalStorage_blockClientIfNeeded(c));
    EXPECT_EQ(dictSize(c->bstate->keys), 1u);

    externalStorage_processCompletedRequests();
    EXPECT_FALSE(isBlocked(c));
}

TEST_F(BlockClientTest, StaysBlockedUntilTheLastKeyIsFetched) {
    addFlashKey("a");
    robj *b = addFlashKey("b");
    setStorageState(b, STORAGE_STATE_COPYING_TO_MEMORY); /* its fetch hasn't been submitted yet */
    client *c = makeClient("mget a b");

    EXPECT_TRUE(externalStorage_blockClientIfNeeded(c));
    externalStorage_processCompletedRequests();
    EXPECT_TRUE(isBlocked(c)); /* a is back, b is not */

    startFetch(b);
    externalStorage_processCompletedRequests();
    EXPECT_FALSE(isBlocked(c));
}

TEST_F(BlockClientTest, AKeyBeingCopiedToMemoryBlocksWithoutASecondFetch) {
    robj *a = addFlashKey("a");
    startFetch(a);
    client *c = makeClient("get a");

    EXPECT_TRUE(externalStorage_blockClientIfNeeded(c));
    EXPECT_TRUE(isBlocked(c));

    externalStorage_processCompletedRequests();
    EXPECT_FALSE(isBlocked(c));
    EXPECT_EQ(numFetched(), 1u);
}

TEST_F(BlockClientTest, AKeyBeingCopiedToFlashDoesNotBlockARead) {
    robj *a = addRawKey("a");
    setStorageState(a, STORAGE_STATE_COPYING_TO_FLASH); /* the value is still in memory */
    client *c = makeClient("get a");

    EXPECT_FALSE(externalStorage_blockClientIfNeeded(c));
    setStorageState(a, STORAGE_STATE_ONLY_MEMORY);
}

#endif /* USE_EXT_STORAGE */
