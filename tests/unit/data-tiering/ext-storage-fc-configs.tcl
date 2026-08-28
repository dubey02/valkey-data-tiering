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

    test "INFO fc_total_disk_write_bytes tracks actual disk writes" {
        # This previously asserted write_bytes > 0 immediately after init, with
        # the rationale "FC writes during init (zero-fill)". That only passed
        # because the INFO field read FC_TOTAL_DB_SIZE_BYTES through a
        # mismatched metric id, reporting the DB file size rather than bytes
        # written. With the correct metric, write bytes start at 0.
        #
        # Two things gate a real disk write. FlashCache buffers spills and only
        # flushes once ext-storage-buffered-write-flush-threshold (1 MiB) is
        # crossed, and values are LZF compressed on the way down, so a repeated
        # character string collapses to almost nothing and never reaches the
        # threshold. Use incompressible payloads.
        regexp {fc_total_disk_write_bytes:(\d+)} [r info all] _ before

        set n 400
        for {set i 0} {$i < $n} {incr i} {
            set v ""
            set x [expr {$i + 7}]
            while {[string length $v] < 8192} {
                set x [expr {($x * 1103515245 + 12345) & 0x7fffffff}]
                append v [format %08x $x]
            }
            r set fcw:$i $v
        }
        regexp {total_num_items_spilled_to_ext_storage:(\d+)} [r info all] _ spilled_before
        for {set i 0} {$i < $n} {incr i} { catch {r debug spill fcw:$i} }

        set deadline [expr {[clock milliseconds] + 15000}]
        while {[clock milliseconds] < $deadline} {
            regexp {total_num_items_spilled_to_ext_storage:(\d+)} [r info all] _ spilled_now
            if {$spilled_now - $spilled_before >= [expr {$n / 2}]} break
            after 100
        }

        regexp {fc_total_disk_write_bytes:(\d+)} [r info all] _ after
        assert {$after > $before}
    }
}

# Cleanup
catch {exec rm -f $flash_path}
