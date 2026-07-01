# Data tiering config tests via MODULE path (flash-tiering module).
# Verifies CONFIG GET/SET works identically for both native and module backends.
#
# Requires: cd modules/flash-tiering && cargo build --release --features backend-flashcache
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-module-configs

set testmodule [file normalize modules/flash-tiering/target/release/libflash_tiering_module.so]

if {![file exists $testmodule]} {
    puts "SKIPPED: flash-tiering module not built at $testmodule"
    puts "Build with: cd modules/flash-tiering && cargo build --release --features backend-flashcache"
    return
}

set flash_path "/tmp/fc-module-configs-[pid].db"
catch {exec fallocate -l 256M $flash_path}

start_server [list tags {"ext-storage-module-configs"} overrides [list \
    ext-storage-enabled yes \
    maxmemory 8mb \
    maxmemory-policy allkeys-lru \
    loadmodule "$testmodule backend=flashcache db_path=$flash_path db_size_bytes=268435456" \
]] {
    # ─── IMMUTABLE configs: verify defaults ─────────────────────────────────
    test "Module: ext-storage-index-size default is 1048576" {
        assert_equal {ext-storage-index-size 1048576} [r config get ext-storage-index-size]
    }

    test "Module: ext-storage-max-allocated-percent default is 90" {
        assert_equal {ext-storage-max-allocated-percent 90} [r config get ext-storage-max-allocated-percent]
    }

    test "Module: ext-storage-max-in-flight-reads default is 128" {
        assert_equal {ext-storage-max-in-flight-reads 128} [r config get ext-storage-max-in-flight-reads]
    }

    # ─── IMMUTABLE configs: reject CONFIG SET ───────────────────────────────
    test "Module: ext-storage-index-size rejects CONFIG SET" {
        catch {r config set ext-storage-index-size 2000000} err
        assert_match "*can't set immutable config*" $err
    }

    test "Module: ext-storage-max-allocated-percent rejects CONFIG SET" {
        catch {r config set ext-storage-max-allocated-percent 80} err
        assert_match "*can't set immutable config*" $err
    }

    test "Module: ext-storage-max-in-flight-reads rejects CONFIG SET" {
        catch {r config set ext-storage-max-in-flight-reads 64} err
        assert_match "*can't set immutable config*" $err
    }

    # ─── MODIFIABLE configs: verify defaults ────────────────────────────────
    test "Module: ext-storage-min-gc-rate default is 4096" {
        assert_equal {ext-storage-min-gc-rate 4096} [r config get ext-storage-min-gc-rate]
    }

    test "Module: ext-storage-max-gc-rate default is 31457280 (30MB)" {
        assert_equal {ext-storage-max-gc-rate 31457280} [r config get ext-storage-max-gc-rate]
    }

    test "Module: ext-storage-max-buffered-write-size default is 4194304 (4MB)" {
        assert_equal {ext-storage-max-buffered-write-size 4194304} [r config get ext-storage-max-buffered-write-size]
    }

    test "Module: ext-storage-buffered-write-flush-threshold default is 1048576 (1MB)" {
        assert_equal {ext-storage-buffered-write-flush-threshold 1048576} [r config get ext-storage-buffered-write-flush-threshold]
    }

    test "Module: ext-storage-max-spill-size default is 134217728 (128MB)" {
        assert_equal {ext-storage-max-spill-size 134217728} [r config get ext-storage-max-spill-size]
    }

    # ─── MODIFIABLE configs: CONFIG SET works ───────────────────────────────
    test "Module: CONFIG SET ext-storage-min-gc-rate propagates" {
        r config set ext-storage-min-gc-rate 8192
        assert_equal {ext-storage-min-gc-rate 8192} [r config get ext-storage-min-gc-rate]
    }

    test "Module: CONFIG SET ext-storage-max-gc-rate propagates" {
        r config set ext-storage-max-gc-rate 67108864
        assert_equal {ext-storage-max-gc-rate 67108864} [r config get ext-storage-max-gc-rate]
    }

    test "Module: CONFIG SET ext-storage-max-buffered-write-size propagates" {
        r config set ext-storage-max-buffered-write-size 8388608
        assert_equal {ext-storage-max-buffered-write-size 8388608} [r config get ext-storage-max-buffered-write-size]
    }

    test "Module: CONFIG SET ext-storage-buffered-write-flush-threshold propagates" {
        r config set ext-storage-buffered-write-flush-threshold 2097152
        assert_equal {ext-storage-buffered-write-flush-threshold 2097152} [r config get ext-storage-buffered-write-flush-threshold]
    }

    test "Module: CONFIG SET ext-storage-max-spill-size propagates" {
        r config set ext-storage-max-spill-size 268435456
        assert_equal {ext-storage-max-spill-size 268435456} [r config get ext-storage-max-spill-size]
    }
}

# Cleanup
catch {exec rm -f $flash_path}
