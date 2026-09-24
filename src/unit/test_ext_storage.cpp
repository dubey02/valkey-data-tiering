/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>
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
}

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
        engine->close();
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

    ASSERT_EQ(delAndWait(0, key), STORAGE_OK);
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_NOT_FOUND);
    EXPECT_EQ(delAndWait(0, key), STORAGE_NOT_FOUND);

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

    storageStats st = {};
    engine->get_stats(&st);
    EXPECT_EQ(st.total_num_items, 1u);
    EXPECT_EQ(st.total_num_bytes, serializedLen("bb"));

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
    storageStats st = {};
    engine->get_stats(&st);
    EXPECT_EQ(st.total_num_items, 1u);
    EXPECT_EQ(st.total_num_bytes, serializedLen("12345"));

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

    /* Submitted but not executed. The store still holds nothing. */
    storageStats st = {};
    engine->get_stats(&st);
    EXPECT_EQ(st.total_num_items, 0u);

    /* One poll both executes the queued put and delivers its completion. */
    storageCompletion comps[4] = {};
    ASSERT_EQ(engine->poll_completions(comps, 4), 1);
    EXPECT_EQ(comps[0].request_ctx, &token);
    EXPECT_EQ(comps[0].op_type, STORAGE_OP_PUT);
    EXPECT_EQ(comps[0].status, STORAGE_OK);
    EXPECT_EQ(comps[0].stored_bytes, serializedLen("v"));

    engine->get_stats(&st);
    EXPECT_EQ(st.total_num_items, 1u);

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

/* Closing with requests still queued drops them and leaves nothing behind. */
TEST_F(StorageMockTest, ClosingWithQueuedRequestsKeepsNothing) {
    openEngine();
    sds key = sdsnew("k");
    dbEntry *entry = makeEntry(key, "v");

    ASSERT_EQ(engine->put_async(0, key, sdslen(key), entry, nullptr), STORAGE_OK);
    ASSERT_EQ(engine->put_async(0, key, sdslen(key), entry, nullptr), STORAGE_OK);
    engine->close();

    openEngine();
    storageStats st = {};
    engine->get_stats(&st);
    EXPECT_EQ(st.total_num_items, 0u);
    EXPECT_EQ(st.total_num_bytes, 0u);

    robj *out = nullptr;
    EXPECT_EQ(getAndWait(0, key, &out), STORAGE_NOT_FOUND);

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
    storageStats st = {};
    engine->get_stats(&st);
    EXPECT_EQ(st.total_num_items, 2u);

    /* A second tick executes the remainder. */
    ASSERT_EQ(storageMockRunIo(8), 1);
    engine->get_stats(&st);
    EXPECT_EQ(st.total_num_items, 3u);

    /* All three completions drain in submission order. */
    storageCompletion comps[8] = {};
    ASSERT_EQ(engine->poll_completions(comps, 8), 3);
    EXPECT_EQ(comps[0].request_ctx, &t1);
    EXPECT_EQ(comps[1].request_ctx, &t2);
    EXPECT_EQ(comps[2].request_ctx, &t3);

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

#endif /* USE_EXT_STORAGE */
