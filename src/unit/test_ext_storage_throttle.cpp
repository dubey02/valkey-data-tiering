/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for ext_storage_throttle.h
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "server.h"
}

#ifdef USE_EXT_STORAGE

extern "C" {
#include "ext_storage.h"
#include "ext_storage_throttle.h"
void setStorageState(robj *o, storageState state);
}

/* Key extraction for a test command whose only key is argv[1]. */
static int firstArgIsKey(struct serverCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    UNUSED(argv);
    if (argc < 2) return 0;
    keyReference *keys = getKeysPrepareResult(result, 1);
    keys[0].pos = 1;
    keys[0].flags = 0;
    result->numkeys = 1;
    return 1;
}

class ExtStorageThrottleTest : public ::testing::Test {
  protected:
    MockValkey mock;
    throttler *dummy_throttler = (throttler *)1;
    bool engine_open = false;

    void SetUp() override {
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
        server.dbnum = 1;
        server.logfile = zstrdup("");
        server.ext_storage_enabled = 1;
        server.ext_storage_engine = zstrdup("mock");
        server.ext_storage_path = zstrdup("");
        server.maxmemory = 1000000;
        server.ext_storage_throttle_enabled = 1;
        /* Band from 100 to 120%, with commands checked at any memory level. */
        server.ext_storage_throttle_measure_percent = 0;
        server.ext_storage_throttle_start_percent = 100;
        server.ext_storage_throttle_max_percent = 120;
        server.ext_storage_throttle_min_baseline = 0;
        storageUnregisterEngine();
        extStorageInit();
        engine_open = true;

        /* The throttler is a stand-in, so the framework calls on it are stubbed. */
        EXPECT_CALL(mock, throttle_deregister(_)).WillRepeatedly(Return());
        EXPECT_CALL(mock, throttle_setRate(_, _)).WillRepeatedly(Return());
        setIncoming(0);
        setUsedPercent(100);
    }

    void TearDown() override {
        /* Disabling the throttle removes any throttler left installed. */
        server.ext_storage_throttle_enabled = 0;
        extStorageThrottleAdjust();
        if (engine_open) extStorageDeinit();
        zfree(server.ext_storage_engine);
        zfree(server.ext_storage_path);
        zfree(server.logfile);
    }

    void setUsedPercent(double percent) {
        size_t used = (size_t)(percent * (double)server.maxmemory / 100.0);
        EXPECT_CALL(mock, getMaxmemoryState(_, _, _, _))
            .WillRepeatedly(DoAll(SetArgPointee<0>(used), Return(C_OK)));
    }

    void setMetrics(double ops_per_sec, int queued_clients) {
        throttleMetrics m = {};
        m.incoming_tps = ops_per_sec;
        m.num_clients_throttled = queued_clients;
        EXPECT_CALL(mock, throttle_getMetrics(_, _)).WillRepeatedly(SetArgPointee<1>(m));
    }

    void setIncoming(double ops_per_sec) {
        setMetrics(ops_per_sec, 0);
    }

    /* Registers the throttler below the start and returns its criteria proc. */
    throttleCriteriaProc *install(double incoming) {
        throttleCriteriaProc *criteria = NULL;
        EXPECT_CALL(mock, throttle_register(_, _, _))
            .WillOnce(DoAll(SaveArg<0>(&criteria), Return(dummy_throttler)));
        setIncoming(incoming);
        setUsedPercent(95);
        extStorageThrottleAdjust();
        return criteria;
    }

    double readMetric(const char *key) {
        sds info = extStorageThrottleInfoString(sdsempty());
        char search_for[128];
        snprintf(search_for, sizeof(search_for), "%s:", key);
        char *p = strstr(info, search_for);
        double v = p ? strtod(p + strlen(search_for), NULL) : -2.0;
        sdsfree(info);
        return v;
    }
};

TEST_F(ExtStorageThrottleTest, DisabledNeverRegisters) {
    server.ext_storage_throttle_enabled = 0;
    setUsedPercent(150);
    EXPECT_CALL(mock, throttle_register(_, _, _)).Times(0);
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_rate"), -1.0);
}

TEST_F(ExtStorageThrottleTest, NoStorageEngineNeverRegisters) {
    extStorageDeinit();
    engine_open = false;
    setUsedPercent(150);
    EXPECT_CALL(mock, throttle_register(_, _, _)).Times(0);
    extStorageThrottleAdjust();
}

/* Below the start the rate is unlimited and the incoming rate becomes the
 * baseline. */
TEST_F(ExtStorageThrottleTest, BelowTheStartMeasuresWithoutThrottling) {
    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, _)).Times(0);
    install(5000);
    EXPECT_EQ(readMetric("ext_storage_throttle_rate"), -1.0);
    EXPECT_EQ(readMetric("ext_storage_throttle_fraction"), 0.0);
    EXPECT_EQ(readMetric("ext_storage_throttle_baseline_ops_per_sec"), 5000.0);
    EXPECT_DOUBLE_EQ(readMetric("ext_storage_throttle_memory_percent"), 95.0);
}

/* Inside the band the cut is proportional to used memory past the start, and
 * applies to the rate measured before throttling began. */
TEST_F(ExtStorageThrottleTest, ThrottlesInProportionToTheBand) {
    install(10000);
    double activations = readMetric("ext_storage_throttle_activation_events");

    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, DoubleNear(5000, 1e-6))).Times(1);
    setUsedPercent(110);
    extStorageThrottleAdjust();
    EXPECT_NEAR(readMetric("ext_storage_throttle_fraction"), 0.5, 1e-4);
    EXPECT_NEAR(readMetric("ext_storage_throttle_rate"), 5000, 1e-2);
    EXPECT_EQ(readMetric("ext_storage_throttle_activation_events"), activations + 1);

    /* Queued clients stop sending, so the incoming rate drops. The baseline holds. */
    setIncoming(3000);
    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, DoubleNear(2500, 1e-6))).Times(1);
    setUsedPercent(115);
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_baseline_ops_per_sec"), 10000.0);
    EXPECT_EQ(readMetric("ext_storage_throttle_activation_events"), activations + 1);
}

TEST_F(ExtStorageThrottleTest, AtTheFullPercentAllowsNothing) {
    install(10000);
    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, 0.0)).Times(1);
    setUsedPercent(125);
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_fraction"), 1.0);
}

/* Back under the start with clients held, the rate rises step by step, then
 * goes unlimited once none are held. The baseline stays frozen until then. */
TEST_F(ExtStorageThrottleTest, BelowTheStartWithHeldClientsRaisesTheRateGradually) {
    install(10000);
    setUsedPercent(110);
    extStorageThrottleAdjust();

    setMetrics(4000, 3);
    setUsedPercent(98);
    EXPECT_CALL(mock, throttle_adjustRate(dummy_throttler, DoubleEq(1.1)))
        .WillOnce(Return(5500.0))
        .WillOnce(Return(6050.0));
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_rate"), 5500.0);
    EXPECT_EQ(readMetric("ext_storage_throttle_fraction"), 0.0);
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_rate"), 6050.0);
    EXPECT_EQ(readMetric("ext_storage_throttle_baseline_ops_per_sec"), 10000.0);

    setMetrics(6000, 0);
    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, THROTTLE_UNLIMITED_RATE)).Times(1);
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_rate"), -1.0);
    EXPECT_EQ(readMetric("ext_storage_throttle_baseline_ops_per_sec"), 6000.0);
}

/* Crossing the start again while ramping up is the same activation, with the
 * same baseline. */
TEST_F(ExtStorageThrottleTest, BackOverTheStartWhileRampingIsTheSameActivation) {
    install(10000);
    double activations = readMetric("ext_storage_throttle_activation_events");
    setUsedPercent(110);
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_activation_events"), activations + 1);

    setMetrics(4000, 3);
    setUsedPercent(98);
    EXPECT_CALL(mock, throttle_adjustRate(dummy_throttler, DoubleEq(1.1))).WillOnce(Return(5500.0));
    extStorageThrottleAdjust();

    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, DoubleNear(5000, 1e-6))).Times(1);
    setUsedPercent(110);
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_activation_events"), activations + 1);
    EXPECT_NEAR(readMetric("ext_storage_throttle_rate"), 5000, 1e-2);
}

TEST_F(ExtStorageThrottleTest, TheMaxPercentSetsWhereThrottlingIsFull) {
    install(4000);
    server.ext_storage_throttle_max_percent = 110;
    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, DoubleNear(1000, 1e-6))).Times(1);
    setUsedPercent(107.5);
    extStorageThrottleAdjust();
    EXPECT_NEAR(readMetric("ext_storage_throttle_fraction"), 0.75, 1e-4);
}

TEST_F(ExtStorageThrottleTest, TheStartPercentSetsWhereThrottlingStarts) {
    server.ext_storage_throttle_start_percent = 110;
    install(10000);
    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, _)).Times(0);
    setUsedPercent(105);
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_fraction"), 0.0);

    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, DoubleNear(5000, 1e-6))).Times(1);
    setUsedPercent(115);
    extStorageThrottleAdjust();
    EXPECT_NEAR(readMetric("ext_storage_throttle_fraction"), 0.5, 1e-4);
}

/* With nothing measured and no floor, the baseline is 0, so every matching
 * command is held at any fraction. */
TEST_F(ExtStorageThrottleTest, AZeroBaselineHoldsEveryMatchingCommand) {
    install(0);
    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, 0.0)).Times(1);
    setUsedPercent(105);
    extStorageThrottleAdjust();
    EXPECT_NEAR(readMetric("ext_storage_throttle_fraction"), 0.25, 1e-4);
}

/* The floor applies when too little traffic was measured. */
TEST_F(ExtStorageThrottleTest, TheMinBaselineFloorsTheBaseline) {
    server.ext_storage_throttle_min_baseline = 2000;
    install(0);
    EXPECT_EQ(readMetric("ext_storage_throttle_baseline_ops_per_sec"), 2000.0);

    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, DoubleNear(1000, 1e-6))).Times(1);
    setUsedPercent(110);
    extStorageThrottleAdjust();
}

/* Below the measure percent nothing matches, writes included. */
TEST_F(ExtStorageThrottleTest, BelowTheMeasurePercentNoCommandMatches) {
    server.ext_storage_throttle_measure_percent = 100;
    throttleCriteriaProc *criteria = install(1000);
    ASSERT_NE(criteria, nullptr);
    struct serverCommand write_cmd;
    memset(&write_cmd, 0, sizeof(write_cmd));
    write_cmd.flags = CMD_WRITE | CMD_DENYOOM;
    client *c = (client *)zcalloc(sizeof(client));
    c->cmd = &write_cmd;

    EXPECT_FALSE(criteria(c, NULL));
    setUsedPercent(101);
    extStorageThrottleAdjust();
    EXPECT_TRUE(criteria(c, NULL));

    zfree(c);
}

TEST_F(ExtStorageThrottleTest, NoInfoWithoutAStorageEngine) {
    extStorageDeinit();
    engine_open = false;
    sds info = extStorageThrottleInfoString(sdsempty());
    EXPECT_EQ(sdslen(info), 0u);
    sdsfree(info);
}

TEST_F(ExtStorageThrottleTest, NoMaxmemoryRemovesTheThrottler) {
    install(1000);
    EXPECT_CALL(mock, throttle_deregister(dummy_throttler)).Times(1);
    server.maxmemory = 0;
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_rate"), -1.0);
}

TEST_F(ExtStorageThrottleTest, DisablingRemovesTheThrottler) {
    install(1000);
    EXPECT_CALL(mock, throttle_deregister(dummy_throttler)).Times(1);
    server.ext_storage_throttle_enabled = 0;
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_rate"), -1.0);
}

TEST_F(ExtStorageThrottleTest, ClosingTheStorageEngineRemovesTheThrottler) {
    install(1000);
    extStorageDeinit();
    engine_open = false;
    EXPECT_CALL(mock, throttle_deregister(dummy_throttler)).Times(1);
    extStorageThrottleAdjust();
}

/* Writes that may grow memory and scripts match. Keyless commands and the
 * primary's stream don't. */
TEST_F(ExtStorageThrottleTest, OnlyCommandsThatMayGrowMemoryAreThrottled) {
    throttleCriteriaProc *criteria = install(1000);
    ASSERT_NE(criteria, nullptr);

    struct serverCommand write_cmd, read_cmd, del_cmd, eval_cmd, evalsha_cmd, fcall_cmd;
    memset(&write_cmd, 0, sizeof(write_cmd));
    memset(&read_cmd, 0, sizeof(read_cmd));
    memset(&del_cmd, 0, sizeof(del_cmd));
    memset(&eval_cmd, 0, sizeof(eval_cmd));
    memset(&evalsha_cmd, 0, sizeof(evalsha_cmd));
    memset(&fcall_cmd, 0, sizeof(fcall_cmd));
    write_cmd.flags = CMD_WRITE | CMD_DENYOOM;
    read_cmd.flags = CMD_READONLY;
    del_cmd.flags = CMD_WRITE;
    eval_cmd.proc = evalCommand;
    evalsha_cmd.proc = evalShaCommand;
    fcall_cmd.proc = fcallCommand;
    client *c = (client *)zcalloc(sizeof(client));

    c->cmd = &write_cmd;
    EXPECT_TRUE(criteria(c, NULL));
    c->cmd = &read_cmd;
    EXPECT_FALSE(criteria(c, NULL));
    c->cmd = &del_cmd;
    EXPECT_FALSE(criteria(c, NULL));
    c->cmd = &eval_cmd;
    EXPECT_TRUE(criteria(c, NULL));
    c->cmd = &evalsha_cmd;
    EXPECT_TRUE(criteria(c, NULL));
    c->cmd = &fcall_cmd;
    EXPECT_TRUE(criteria(c, NULL));
    c->cmd = &write_cmd;
    c->flag.primary = 1;
    EXPECT_FALSE(criteria(c, NULL));
    c->flag.primary = 0;
    c->id = CLIENT_ID_AOF;
    EXPECT_FALSE(criteria(c, NULL));

    zfree(c);
}

/* Other commands match only when a key's value is not in memory. A missing
 * key doesn't match. */
TEST_F(ExtStorageThrottleTest, CommandsAreThrottledOnlyForValuesNotInMemory) {
    throttleCriteriaProc *criteria = install(1000);
    ASSERT_NE(criteria, nullptr);

    serverDb *db = (serverDb *)zcalloc(sizeof(serverDb));
    db->keys = kvstoreCreate(&kvstoreKeysHashtableType, 0, 0);
    db->expires = kvstoreCreate(&kvstoreExpiresHashtableType, 0, 0);
    const char *names[] = {"in_memory", "on_flash"};
    storageState states[] = {STORAGE_STATE_ONLY_MEMORY, STORAGE_STATE_ONLY_FLASH};
    bool throttled[] = {false, true};
    robj *entries[2];
    for (int i = 0; i < 2; i++) {
        sds key = sdsnew(names[i]);
        entries[i] = objectSetKeyAndExpire(createStringObject("value", 5), key, -1);
        sdsfree(key);
        kvstoreHashtableAdd(db->keys, 0, entries[i]);
    }

    struct serverCommand cmds[2];
    memset(cmds, 0, sizeof(cmds));
    cmds[0].flags = CMD_READONLY;
    cmds[1].flags = CMD_WRITE;
    const char *cmd_names[] = {"read", "write"};
    client *c = (client *)zcalloc(sizeof(client));
    robj *argv[2];
    argv[0] = createStringObject("cmd", 3);
    c->db = db;
    c->argv = argv;
    c->argc = 2;

    for (int j = 0; j < 2; j++) {
        cmds[j].getkeys_proc = firstArgIsKey;
        c->cmd = &cmds[j];
        for (int i = 0; i < 2; i++) {
            setStorageState(entries[i], states[i]);
            argv[1] = createStringObject(names[i], strlen(names[i]));
            EXPECT_EQ(criteria(c, NULL), throttled[i]) << cmd_names[j] << " " << names[i];
            decrRefCount(argv[1]);
            /* Back in memory so the key can be freed. */
            setStorageState(entries[i], STORAGE_STATE_ONLY_MEMORY);
        }
        argv[1] = createStringObject("missing", 7);
        EXPECT_FALSE(criteria(c, NULL)) << cmd_names[j];
        decrRefCount(argv[1]);
    }

    decrRefCount(argv[0]);
    zfree(c);
    kvstoreRelease(db->expires);
    kvstoreRelease(db->keys);
    zfree(db);
}

#endif /* USE_EXT_STORAGE */
