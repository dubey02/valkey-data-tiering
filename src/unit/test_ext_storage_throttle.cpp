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
void extStorageTestResetRegistry(void);
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
        server.ext_storage_throttle_max_percent = 120;
        extStorageTestResetRegistry();
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

    void setIncoming(double ops_per_sec) {
        throttleMetrics m = {};
        m.incoming_tps = ops_per_sec;
        EXPECT_CALL(mock, throttle_getMetrics(_, _)).WillRepeatedly(SetArgPointee<1>(m));
    }

    /* Registers the throttler below the throttle start, where it measures the
     * incoming rate without cutting, and returns its criteria proc. */
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

/* Below the throttle start the throttler stays registered at the unlimited
 * rate, and the incoming rate becomes the baseline. */
TEST_F(ExtStorageThrottleTest, BelowTheStartMeasuresWithoutCutting) {
    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, _)).Times(0);
    install(5000);
    EXPECT_EQ(readMetric("ext_storage_throttle_rate"), -1.0);
    EXPECT_EQ(readMetric("ext_storage_throttle_fraction"), 0.0);
    EXPECT_EQ(readMetric("ext_storage_throttle_baseline_ops_per_sec"), 5000.0);
    EXPECT_DOUBLE_EQ(readMetric("ext_storage_throttle_memory_percent"), 95.0);
}

/* Inside the band the cut is proportional to how far used memory is past the
 * start, applied to the baseline measured before cutting began. */
TEST_F(ExtStorageThrottleTest, CutsInProportionToTheBand) {
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

/* Back under the start, the rate returns to unlimited and the baseline follows
 * the incoming rate again. */
TEST_F(ExtStorageThrottleTest, BelowTheStartAgainStopsCutting) {
    install(10000);
    setUsedPercent(110);
    extStorageThrottleAdjust();

    setIncoming(8000);
    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, THROTTLE_UNLIMITED_RATE)).Times(1);
    setUsedPercent(98);
    extStorageThrottleAdjust();
    EXPECT_EQ(readMetric("ext_storage_throttle_rate"), -1.0);
    EXPECT_EQ(readMetric("ext_storage_throttle_fraction"), 0.0);
    EXPECT_EQ(readMetric("ext_storage_throttle_baseline_ops_per_sec"), 8000.0);
}

TEST_F(ExtStorageThrottleTest, TheMaxPercentSetsTheEndOfTheCut) {
    install(4000);
    server.ext_storage_throttle_max_percent = 110;
    EXPECT_CALL(mock, throttle_setRate(dummy_throttler, DoubleNear(1000, 1e-6))).Times(1);
    setUsedPercent(107.5);
    extStorageThrottleAdjust();
    EXPECT_NEAR(readMetric("ext_storage_throttle_fraction"), 0.75, 1e-4);
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

/* Writes that may grow memory and reads are slowed. Other commands go through,
 * and so does the primary's replication stream. */
TEST_F(ExtStorageThrottleTest, OnlyCommandsThatMayGrowMemoryAreThrottled) {
    throttleCriteriaProc *criteria = install(1000);
    ASSERT_NE(criteria, nullptr);

    struct serverCommand write_cmd, read_cmd, del_cmd;
    memset(&write_cmd, 0, sizeof(write_cmd));
    memset(&read_cmd, 0, sizeof(read_cmd));
    memset(&del_cmd, 0, sizeof(del_cmd));
    write_cmd.flags = CMD_WRITE | CMD_DENYOOM;
    read_cmd.flags = CMD_READONLY;
    del_cmd.flags = CMD_WRITE;
    client *c = (client *)zcalloc(sizeof(client));

    c->cmd = &write_cmd;
    EXPECT_TRUE(criteria(c, NULL));
    c->cmd = &read_cmd;
    EXPECT_TRUE(criteria(c, NULL));
    c->cmd = &del_cmd;
    EXPECT_FALSE(criteria(c, NULL));
    c->cmd = &write_cmd;
    c->flag.primary = 1;
    EXPECT_FALSE(criteria(c, NULL));

    zfree(c);
}

#endif /* USE_EXT_STORAGE */
