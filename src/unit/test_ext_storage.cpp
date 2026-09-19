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
#include "storage/storage.h"
#include "storage/storage_mock.h"
}

/* ---------------------------------------------------------------------------
 * Mock storage engine
 *
 * These tests drive the mock storage engine with the real serializer, so keys
 * are sds and values are robj that round-trip through the actual DUMP encoding.
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

    /* Waits for one completed request. The storage engine is async only, so a
     * test submits a request and then waits for its completion. This mirrors an
     * IO worker's waitForCompletedRequest. Where a real worker sleeps while a
     * separate IO thread makes progress, here each attempt runs one IO tick,
     * the stand-in for that thread, then tries to drain a completion. Returns
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
    storageStatus putAndWait(uint32_t db_id, void *key, robj *val, size_t *stored_bytes) {
        storageStatus submit = engine->put_async(db_id, key, val, nullptr);
        if (submit != STORAGE_OK) return submit;
        storageCompletion comp = {};
        EXPECT_EQ(waitForCompletedRequest(&comp), 1);
        if (stored_bytes) *stored_bytes = comp.stored_bytes;
        return comp.status;
    }

    /* Gets a value. On STORAGE_OK, *out receives the value object the caller
     * must release. */
    storageStatus getAndWait(uint32_t db_id, void *key, robj **out) {
        *out = nullptr;
        storageStatus submit = engine->get_async(db_id, key, nullptr);
        if (submit != STORAGE_OK) return submit;
        storageCompletion comp = {};
        EXPECT_EQ(waitForCompletedRequest(&comp), 1);
        if (comp.status == STORAGE_OK) *out = (robj *)comp.val_obj;
        return comp.status;
    }

    /* Deletes a key and returns the completion status. */
    storageStatus delAndWait(uint32_t db_id, void *key) {
        storageStatus submit = engine->del_async(db_id, key, nullptr);
        if (submit != STORAGE_OK) return submit;
        storageCompletion comp = {};
        EXPECT_EQ(waitForCompletedRequest(&comp), 1);
        return comp.status;
    }

    void TearDown() override {
        engine->close();
        extStorageTestResetRegistry();
        zfree(server.logfile);
        server.logfile = NULL;
    }

    /* Serialized DUMP length of a string value, for byte-accounting asserts. */
    size_t serializedLen(const char *s) {
        robj *o = createStringObject(s, strlen(s));
        char *bytes = nullptr;
        int len = cfg.serializer.serialize_value(o, &bytes);
        cfg.serializer.free_serialized_value(bytes);
        decrRefCount(o);
        return (size_t)len;
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
    cfg.serializer.serialize_value = nullptr;
    EXPECT_EQ(engine->open(&cfg), STORAGE_ERR_REJECTED);
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
    robj *val = createStringObject("hello", 5);
    size_t stored = 0;

    ASSERT_EQ(putAndWait(0, key, val, &stored), STORAGE_OK);
    EXPECT_EQ(stored, serializedLen("hello"));

    robj *out = nullptr;
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal(out), "hello"), 0);
    decrRefCount(out);

    ASSERT_EQ(delAndWait(0, key), STORAGE_OK);
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_NOT_FOUND);
    EXPECT_EQ(delAndWait(0, key), STORAGE_NOT_FOUND);

    sdsfree(key);
    decrRefCount(val);
}

/* An integer value is OBJ_ENCODING_INT, not an sds. It must round-trip through
 * the real serializer as its numeric value. */
TEST_F(StorageMockTest, IntEncodedValueRoundTrips) {
    openEngine();
    sds key = sdsnew("counter");
    robj *val = createStringObjectFromLongLong(1234567);
    ASSERT_EQ(val->encoding, OBJ_ENCODING_INT);

    ASSERT_EQ(putAndWait(0, key, val, nullptr), STORAGE_OK);

    robj *out = nullptr;
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    long long v = 0;
    ASSERT_EQ(getLongLongFromObject(out, &v), C_OK);
    EXPECT_EQ(v, 1234567);
    decrRefCount(out);

    sdsfree(key);
    decrRefCount(val);
}

/* The same key in two databases is two records. */
TEST_F(StorageMockTest, KeysAreScopedPerDatabase) {
    openEngine();
    sds key = sdsnew("same");
    robj *v0 = createStringObject("in-db-0", 7);
    robj *v2 = createStringObject("in-db-2", 7);

    ASSERT_EQ(putAndWait(0, key, v0, nullptr), STORAGE_OK);
    ASSERT_EQ(putAndWait(2, key, v2, nullptr), STORAGE_OK);

    robj *out = nullptr;
    ASSERT_EQ(getAndWait(2, key, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal(out), "in-db-2"), 0);
    decrRefCount(out);

    ASSERT_EQ(delAndWait(2, key), STORAGE_OK);
    ASSERT_EQ(getAndWait(0, key, &out), STORAGE_OK);
    decrRefCount(out);

    sdsfree(key);
    decrRefCount(v0);
    decrRefCount(v2);
}

TEST_F(StorageMockTest, RejectsOutOfRangeDatabase) {
    openEngine();
    sds key = sdsnew("k");
    robj *val = createStringObject("v", 1);
    EXPECT_EQ(putAndWait(cfg.num_databases, key, val, nullptr), STORAGE_ERR_REJECTED);
    sdsfree(key);
    decrRefCount(val);
}

TEST_F(StorageMockTest, OverwriteReplacesRatherThanAccumulates) {
    openEngine();
    sds key = sdsnew("k");
    robj *first = createStringObject("aaaa", 4);
    robj *second = createStringObject("bb", 2);

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
    robj *five = createStringObject("12345", 5);

    ASSERT_EQ(putAndWait(0, k1, five, nullptr), STORAGE_OK);
    EXPECT_EQ(putAndWait(0, k2, five, nullptr), STORAGE_ERR_FULL);

    /* The rejected write left nothing behind. */
    storageStats st = {};
    engine->get_stats(&st);
    EXPECT_EQ(st.total_num_items, 1u);
    EXPECT_EQ(st.total_num_bytes, serializedLen("12345"));

    sdsfree(k1);
    sdsfree(k2);
    decrRefCount(five);
}

/* An async request is accepted but not executed at submit. Polling executes
 * the queued work and delivers its completion. */
TEST_F(StorageMockTest, AsyncPutIsNotExecutedAtSubmit) {
    openEngine();
    sds key = sdsnew("k");
    robj *val = createStringObject("v", 1);
    int token = 42;

    ASSERT_EQ(engine->put_async(0, key, val, &token), STORAGE_OK);

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
    decrRefCount(val);
}

TEST_F(StorageMockTest, AsyncGetDeliversTheValueOnTheCompletion) {
    openEngine();
    sds key = sdsnew("k");
    robj *val = createStringObject("payload", 7);
    ASSERT_EQ(putAndWait(0, key, val, nullptr), STORAGE_OK);

    int token = 7;
    ASSERT_EQ(engine->get_async(0, key, &token), STORAGE_OK);

    ASSERT_EQ(storageMockRunIo(2), 1);
    storageCompletion comps[2] = {};
    ASSERT_EQ(engine->poll_completions(comps, 2), 1);
    EXPECT_EQ(comps[0].op_type, STORAGE_OP_GET);
    EXPECT_EQ(comps[0].status, STORAGE_OK);
    ASSERT_NE(comps[0].val_obj, nullptr);
    EXPECT_EQ(strcmp((const char *)objectGetVal((robj *)comps[0].val_obj), "payload"), 0);
    decrRefCount((robj *)comps[0].val_obj);

    sdsfree(key);
    decrRefCount(val);
}

TEST_F(StorageMockTest, AsyncMissReportsNotFoundAndNoValue) {
    openEngine();
    sds key = sdsnew("absent");
    int token = 0;
    ASSERT_EQ(engine->get_async(0, key, &token), STORAGE_OK);

    ASSERT_EQ(storageMockRunIo(1), 1);
    storageCompletion comps[1] = {};
    ASSERT_EQ(engine->poll_completions(comps, 1), 1);
    EXPECT_EQ(comps[0].status, STORAGE_NOT_FOUND);
    EXPECT_EQ(comps[0].val_obj, nullptr);

    sdsfree(key);
}

TEST_F(StorageMockTest, CompletionsArriveInSubmissionOrder) {
    openEngine();
    sds k1 = sdsnew("k1");
    sds k2 = sdsnew("k2");
    robj *v = createStringObject("v", 1);
    int t1 = 1, t2 = 2;

    ASSERT_EQ(engine->put_async(0, k1, v, &t1), STORAGE_OK);
    ASSERT_EQ(engine->put_async(0, k2, v, &t2), STORAGE_OK);

    /* One IO tick executes both queued requests in submission order. */
    ASSERT_EQ(storageMockRunIo(8), 2);
    storageCompletion comps[8] = {};
    ASSERT_EQ(engine->poll_completions(comps, 8), 2);
    EXPECT_EQ(comps[0].request_ctx, &t1);
    EXPECT_EQ(comps[1].request_ctx, &t2);

    sdsfree(k1);
    sdsfree(k2);
    decrRefCount(v);
}

/* A full request queue must return WOULDBLOCK so the caller can back off and
 * retry. Running the IO thread and draining the completions makes room. */
TEST_F(StorageMockTest, ReportsWouldBlockWhenTheQueueIsFull) {
    openEngine();
    sds key = sdsnew("k");
    robj *val = createStringObject("v", 1);

    int accepted = 0;
    for (int i = 0; i < 1000; i++) {
        if (engine->get_async(0, key, &accepted) != STORAGE_OK) break;
        accepted++;
    }
    ASSERT_GT(accepted, 0);
    EXPECT_EQ(engine->get_async(0, key, &accepted), STORAGE_WOULDBLOCK);

    /* Execute the queued requests, then drain the completions. Both steps are
     * needed to free the request ring. */
    ASSERT_GT(storageMockRunIo(1000), 0);
    storageCompletion comps[256] = {};
    int drained = engine->poll_completions(comps, 256);
    ASSERT_GT(drained, 0);
    for (int i = 0; i < drained; i++)
        if (comps[i].val_obj) decrRefCount((robj *)comps[i].val_obj);
    EXPECT_EQ(engine->put_async(0, key, val, &accepted), STORAGE_OK);

    sdsfree(key);
    decrRefCount(val);
}

TEST_F(StorageMockTest, PollingAnIdleEngineReturnsNothing) {
    openEngine();
    storageCompletion comps[4] = {};
    EXPECT_EQ(engine->poll_completions(comps, 4), 0);
}

/* An IO tick executes only up to its budget. The rest stay queued for a later
 * tick. */
TEST_F(StorageMockTest, IoTickExecutesOnlyUpToItsBudget) {
    openEngine();
    sds k1 = sdsnew("k1");
    sds k2 = sdsnew("k2");
    sds k3 = sdsnew("k3");
    robj *v = createStringObject("v", 1);
    int t1 = 1, t2 = 2, t3 = 3;

    ASSERT_EQ(engine->put_async(0, k1, v, &t1), STORAGE_OK);
    ASSERT_EQ(engine->put_async(0, k2, v, &t2), STORAGE_OK);
    ASSERT_EQ(engine->put_async(0, k3, v, &t3), STORAGE_OK);

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
    decrRefCount(v);
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
};

TEST_F(ExtStorageSerializerTest, StringValueSurvivesTheRoundTrip) {
    const storageSerializer *s = extStorageSerializer();
    robj *o = createStringObject("some value", 10);

    char *bytes = nullptr;
    int len = s->serialize_value(o, &bytes);
    ASSERT_GT(len, 0);
    ASSERT_NE(bytes, nullptr);

    robj *back = (robj *)s->deserialize_value(bytes, len);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ((int)back->type, OBJ_STRING);
    EXPECT_EQ(compareStringObjects(o, back), 0);

    s->free_serialized_value(bytes);
    decrRefCount(o);
    decrRefCount(back);
}

/* Keys serialize by reference. The matching free is a no-op. A storage engine
 * that assumed it owned those bytes would double-free. */
TEST_F(ExtStorageSerializerTest, KeySerializationAliasesTheKey) {
    const storageSerializer *s = extStorageSerializer();
    sds key = sdsnew("mykey");

    char *bytes = nullptr;
    int len = s->serialize_key(key, &bytes);
    ASSERT_EQ(len, 5);
    EXPECT_EQ((void *)bytes, (void *)key);

    s->free_serialized_key(bytes);
    /* Still intact because nothing was released. */
    EXPECT_EQ(memcmp(bytes, "mykey", 5), 0);

    sdsfree(key);
}

TEST_F(ExtStorageSerializerTest, KeyDeserializationProducesAnSds) {
    const storageSerializer *s = extStorageSerializer();
    sds key = (sds)s->deserialize_key("abc", 3);
    ASSERT_NE(key, nullptr);
    EXPECT_EQ(sdslen(key), 3u);
    EXPECT_EQ(memcmp(key, "abc", 3), 0);
    sdsfree(key);
}

TEST_F(ExtStorageSerializerTest, RejectsAMalformedBuffer) {
    const storageSerializer *s = extStorageSerializer();
    EXPECT_EQ(s->deserialize_value("not a dump payload", 18), nullptr);
    EXPECT_EQ(s->deserialize_value("", -1), nullptr);

    // A real payload with its trailing version and CRC chopped off must be
    // rejected, not loaded. This is the case that matters: the bytes parse as a
    // valid type but fail verification.
    robj *o = createStringObject("some value", 10);
    char *bytes = nullptr;
    int len = s->serialize_value(o, &bytes);
    ASSERT_GT(len, 10);
    EXPECT_EQ(s->deserialize_value(bytes, len - 4), nullptr);
    s->free_serialized_value(bytes);
    decrRefCount(o);
}

#endif /* USE_EXT_STORAGE */
