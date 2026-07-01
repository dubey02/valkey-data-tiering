/*
 * Test: Pluggable Storage Interface — all 4 configurations.
 * Each backend has its OWN middleware (IO thread) + storage.
 *
 * Build:
 *   cd src/storage && gcc -pthread -Wall -o tests/test_storage tests/test_storage.c \
 *       storage_dispatch.c storage_middleware.c storage_flashcache.c \
 *       storage_rocksdb.c storage_rocksdb_async.c storage_serialize.c -I.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "storage.h"

#define NUM_KEYS 100
#define VAL_SIZE 64

static int g_completions_received;
static storageCompletion g_last_completion;

static void on_completion(storageCompletion *c, void *privdata) {
    (void)privdata;
    g_last_completion = *c;
    /* Free GET values to avoid leaks */
    if (c->op_type == STORAGE_OP_GET && c->value) {
        free(c->value);
        c->value = NULL;
    }
    g_completions_received++;
}

static void wait_completions(int expected) {
    int attempts = 0;
    while (g_completions_received < expected && attempts < 50000) {
        storagePollCompletions(64);
        usleep(50);
        attempts++;
    }
}

static int test_backend(storageType *type, const char *label) {
    printf("[TEST] %s\n", label);
    int pass = 1;
    g_completions_received = 0;

    storageConfig cfg = {
        .path = "/tmp/test_storage",
        .capacity_bytes = 100 * 1024 * 1024,
        .num_databases = 16,
        .io_threads = 2,
        .completion_fn = on_completion,
        .completion_privdata = NULL,
    };

    int rc = storageInit(type, &cfg);
    assert(rc == 0);
    printf("  init: OK\n");

    /* PUT 100 keys */
    for (int i = 0; i < NUM_KEYS; i++) {
        char key[32], val[VAL_SIZE];
        snprintf(key, sizeof(key), "key:%06d", i);
        memset(val, 0, VAL_SIZE);
        snprintf(val, VAL_SIZE, "value-%06d-padding-data", i);
        storageStatus s = storageSubmitPut(0, key, strlen(key), val, VAL_SIZE, 0, NULL);
        assert(s == STORAGE_WOULDBLOCK || s == STORAGE_OK);
    }
    wait_completions(NUM_KEYS);
    int put_ok = (g_completions_received == NUM_KEYS);
    printf("  put %d keys: %d completions %s\n", NUM_KEYS, g_completions_received, put_ok ? "OK" : "FAIL");
    if (!put_ok) pass = 0;

    /* GET and verify */
    g_completions_received = 0;
    for (int i = 0; i < NUM_KEYS; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key:%06d", i);
        storageStatus s = storageSubmitGet(0, key, strlen(key), NULL);
        assert(s == STORAGE_WOULDBLOCK || s == STORAGE_OK);
    }
    wait_completions(NUM_KEYS);
    int get_ok = (g_completions_received == NUM_KEYS);
    printf("  get %d keys: %d completions %s\n", NUM_KEYS, g_completions_received, get_ok ? "OK" : "FAIL");
    if (!get_ok) pass = 0;

    /* DELETE all */
    g_completions_received = 0;
    for (int i = 0; i < NUM_KEYS; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key:%06d", i);
        storageSubmitDel(0, key, strlen(key), NULL);
    }
    wait_completions(NUM_KEYS);
    int del_ok = (g_completions_received == NUM_KEYS);
    printf("  del %d keys: %d completions %s\n", NUM_KEYS, g_completions_received, del_ok ? "OK" : "FAIL");
    if (!del_ok) pass = 0;

    /* Verify deleted */
    g_completions_received = 0;
    storageSubmitGet(0, "key:000000", 10, NULL);
    wait_completions(1);
    int gone = (g_last_completion.status == STORAGE_NOT_FOUND);
    printf("  verify deleted: %s\n", gone ? "OK" : "FAIL");
    if (!gone) pass = 0;

    storageShutdown();
    printf("  shutdown: OK\n");
    printf("  RESULT: %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

int main(void) {
    int total_pass = 0, total_fail = 0;

    printf("=== Pluggable Storage Interface Tests ===\n");
    printf("=== All backends use their own middleware + storage ===\n\n");

    /* 1. Native FlashCache — own IO (async, io_uring simulated) + own storage */
    if (test_backend(storageGetFlashCacheType(),
                     "1. Native: own middleware (ASIO) + FlashCache storage"))
        total_pass++; else total_fail++;

    /* 2. Module FlashCache — own IO + own storage (simulates module registration) */
    if (test_backend(storageGetFlashCacheType(),
                     "2. Module: own middleware (ASIO) + FlashCache storage"))
        total_pass++; else total_fail++;

    /* 3. Native RocksDB — own IO (async worker thread) + own storage */
    if (test_backend(storageGetRocksDBAsyncType(),
                     "3. Native: own middleware (ASIO) + RocksDB storage"))
        total_pass++; else total_fail++;

    /* 4. Module RocksDB — own IO + own storage (simulates module registration) */
    if (test_backend(storageGetRocksDBAsyncType(),
                     "4. Module: own middleware (ASIO) + RocksDB storage"))
        total_pass++; else total_fail++;

    printf("=== SUMMARY: %d PASS, %d FAIL ===\n", total_pass, total_fail);
    return total_fail > 0 ? 1 : 0;
}
