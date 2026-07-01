# Data tiering FlashCache tuning config tests.
# Verifies: all ext-storage-* FC tuning configs are registered, have correct
# defaults, MODIFIABLE ones accept CONFIG SET, IMMUTABLE ones reject it.
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-fc-configs

set flash_path "/tmp/fc-test-configs-[pid].db"
catch {exec fallocate -l 256M $flash_path}

# ─── Config defaults and mutability tests ───────────────────────────────────

start_server [list tags {"ext-storage-fc-configs"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $flash_path \
    ext-storage-capacity-mb 256 \
    maxmemory 8mb \
    maxmemory-policy allkeys-lru \
]] {
    test "IMMUTABLE: ext-storage-index-size default is 1048576" {
        assert_equal {ext-storage-index-size 1048576} [r config get ext-storage-index-size]
    }

    test "IMMUTABLE: ext-storage-max-allocated-percent default is 90" {
        assert_equal {ext-storage-max-allocated-percent 90} [r config get ext-storage-max-allocated-percent]
    }

    test "IMMUTABLE: ext-storage-max-in-flight-reads default is 128" {
        assert_equal {ext-storage-max-in-flight-reads 128} [r config get ext-storage-max-in-flight-reads]
    }

    test "IMMUTABLE: ext-storage-index-size rejects CONFIG SET" {
        catch {r config set ext-storage-index-size 2000000} err
        assert_match "*can't set immutable config*" $err
    }

    test "IMMUTABLE: ext-storage-max-allocated-percent rejects CONFIG SET" {
        catch {r config set ext-storage-max-allocated-percent 80} err
        assert_match "*can't set immutable config*" $err
    }

    test "IMMUTABLE: ext-storage-max-in-flight-reads rejects CONFIG SET" {
        catch {r config set ext-storage-max-in-flight-reads 64} err
        assert_match "*can't set immutable config*" $err
    }

    test "MODIFIABLE: ext-storage-min-gc-rate default is 4096" {
        assert_equal {ext-storage-min-gc-rate 4096} [r config get ext-storage-min-gc-rate]
    }

    test "MODIFIABLE: ext-storage-max-gc-rate default is 31457280 (30MB)" {
        assert_equal {ext-storage-max-gc-rate 31457280} [r config get ext-storage-max-gc-rate]
    }

    test "MODIFIABLE: ext-storage-max-buffered-write-size default is 4194304 (4MB)" {
        assert_equal {ext-storage-max-buffered-write-size 4194304} [r config get ext-storage-max-buffered-write-size]
    }

    test "MODIFIABLE: ext-storage-buffered-write-flush-threshold default is 1048576 (1MB)" {
        assert_equal {ext-storage-buffered-write-flush-threshold 1048576} [r config get ext-storage-buffered-write-flush-threshold]
    }

    test "MODIFIABLE: CONFIG SET ext-storage-min-gc-rate works" {
        r config set ext-storage-min-gc-rate 8192
        assert_equal {ext-storage-min-gc-rate 8192} [r config get ext-storage-min-gc-rate]
    }

    test "MODIFIABLE: CONFIG SET ext-storage-max-gc-rate works" {
        r config set ext-storage-max-gc-rate 67108864
        assert_equal {ext-storage-max-gc-rate 67108864} [r config get ext-storage-max-gc-rate]
    }

    test "MODIFIABLE: CONFIG SET ext-storage-max-buffered-write-size works" {
        r config set ext-storage-max-buffered-write-size 8388608
        assert_equal {ext-storage-max-buffered-write-size 8388608} [r config get ext-storage-max-buffered-write-size]
    }

    test "MODIFIABLE: CONFIG SET ext-storage-buffered-write-flush-threshold works" {
        r config set ext-storage-buffered-write-flush-threshold 2097152
        assert_equal {ext-storage-buffered-write-flush-threshold 2097152} [r config get ext-storage-buffered-write-flush-threshold]
    }

    test "MODIFIABLE: ext-storage-max-spill-size default is 134217728 (128MB)" {
        assert_equal {ext-storage-max-spill-size 134217728} [r config get ext-storage-max-spill-size]
    }

    test "MODIFIABLE: CONFIG SET ext-storage-max-spill-size works" {
        r config set ext-storage-max-spill-size 268435456
        assert_equal {ext-storage-max-spill-size 268435456} [r config get ext-storage-max-spill-size]
    }

    # ─── FlashCache internal metrics in INFO ────────────────────────────────
    test "INFO contains fc_num_items_evicted metric" {
        set info [r info all]
        assert_match "*fc_num_items_evicted:*" $info
    }

    test "INFO contains fc_total_evicted_bytes metric" {
        set info [r info all]
        assert_match "*fc_total_disk_write_bytes:*" $info
    }

    test "INFO contains fc_num_disk_reads metric" {
        set info [r info all]
        assert_match "*fc_num_disk_reads:*" $info
    }

    test "INFO contains fc_num_retryable_disk_errors metric" {
        set info [r info all]
        assert_match "*fc_num_retryable_disk_errors:*" $info
    }

    test "INFO fc_total_disk_write_bytes is non-zero after FC init" {
        set info [r info all]
        regexp {fc_total_disk_write_bytes:(\d+)} $info _ write_bytes
        # FC writes during init (zero-fill), so this is always > 0
        assert {$write_bytes > 0}
    }
}

# Cleanup
catch {exec rm -f $flash_path}
