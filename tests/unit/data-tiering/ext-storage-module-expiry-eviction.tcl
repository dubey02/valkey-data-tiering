# Data tiering expiry + eviction tests via MODULE path (flash-tiering module).
#
# Requires: cd modules/flash-tiering && cargo build --release --features backend-flashcache
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-module-expiry-eviction

set testmodule [file normalize modules/flash-tiering/target/release/libflash_tiering_module.so]

if {![file exists $testmodule]} {
    puts "SKIPPED: flash-tiering module not built at $testmodule"
    puts "Build with: cd modules/flash-tiering && cargo build --release --features backend-flashcache"
    return
}

set flash_path "/tmp/fc-module-test-[pid].db"
catch {exec fallocate -l 256M $flash_path}

proc wait_for_spill {expected_min {timeout 15000}} {
    set start [clock milliseconds]
    while {1} {
        set info [r info all]
        if {[regexp {total_num_items_spilled_to_ext_storage:(\d+)} $info _ spilled]} {
            if {$spilled >= $expected_min} { return $spilled }
        }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill (got $spilled, wanted $expected_min)"
        }
        after 100
    }
}

proc fill_memory {} {
    set padding [string repeat "X" 1024]
    for {set i 0} {$i < 10000} {incr i} {
        catch {r set "filler:[format %05d $i]" "${padding}_$i"}
    }
}

# ═══════════════════════════════════════════════════════════════════════════════
# EXPIRY TESTS (module path)
# ═══════════════════════════════════════════════════════════════════════════════

start_server [list tags {"ext-storage-module-expiry"} overrides [list \
    ext-storage-enabled yes \
    maxmemory 8mb \
    maxmemory-policy allkeys-lru \
    hz 100 \
    loadmodule "$testmodule backend=flashcache db_path=$flash_path db_size_bytes=268435456" \
]] {

    test {Module: TTL on tiered key returns correct value} {
        r set ttlkey "value-for-ttl-test-padded-1234567890" EX 60
        fill_memory
        wait_for_spill 50
        set ttl [r ttl ttlkey]
        assert {$ttl > 0 && $ttl <= 60}
    }

    test {Module: PTTL on tiered key} {
        set pttl [r pttl ttlkey]
        assert {$pttl > 0 && $pttl <= 60000}
    }

    test {Module: PERSIST removes TTL on tiered key} {
        r set persistkey "persist-value-padded-1234567890" EX 60
        after 200
        r persist persistkey
        assert_equal [r ttl persistkey] -1
    }

    test {Module: EXPIRE sets TTL on tiered key} {
        r set expkey "expire-value-padded-1234567890"
        after 200
        r expire expkey 30
        set ttl [r ttl expkey]
        assert {$ttl > 0 && $ttl <= 30}
    }

    test {Module: passive expiry - GET on expired tiered key returns empty} {
        r flushdb
        after 1000
        r set shortlived "value-that-will-expire-on-flash-padding" EX 4
        fill_memory
        wait_for_spill 50
        assert_equal [r exists shortlived] 1
        after 4500
        assert_equal [r get shortlived] {}
    }

    test {Module: passive expiry - EXISTS returns 0 for expired tiered key} {
        assert_equal [r exists shortlived] 0
    }

    test {Module: active expiry removes tiered keys} {
        r flushdb
        after 1000
        for {set i 0} {$i < 50} {incr i} {
            r set "ae:$i" "active-expiry-value-$i-padded-for-size" EX 4
        }
        fill_memory
        wait_for_spill 50
        set before [r dbsize]
        after 5000
        set after [r dbsize]
        set expired [expr {$before - $after}]
        assert {$expired >= 45}
    }

    test {Module: server alive after expiry (no crash)} {
        assert_equal [r ping] PONG
    }

    test {Module: FLUSHDB clears tiered keys} {
        r flushdb
        after 1000
        fill_memory
        wait_for_spill 10
        assert {[r dbsize] > 0}
        r flushdb
        after 500
        assert_equal [r dbsize] 0
    }

    test {Module: FLUSHALL clears tiered keys} {
        r flushdb
        after 1000
        fill_memory
        wait_for_spill 10
        assert {[r dbsize] > 0}
        r flushall
        after 500
        assert_equal [r dbsize] 0
        assert_equal [r ping] PONG
    }

    test {Module: key expires during fetch (no wasted promotion)} {
        r flushdb
        after 1000
        r set fetchexpire "value-that-will-expire-during-fetch-padding" EX 8
        fill_memory
        wait_for_spill 50
        assert_equal [r exists fetchexpire] 1
        after 8500
        assert_equal [r get fetchexpire] {}
        assert_equal [r exists fetchexpire] 0
    }

    test {Module: server alive after all expiry tests (no crash)} {
        assert_equal [r ping] PONG
    }
}

# ═══════════════════════════════════════════════════════════════════════════════
# EVICTION TESTS (module path)
# ═══════════════════════════════════════════════════════════════════════════════

set flash_path2 "/tmp/fc-module-test2-[pid].db"
catch {exec fallocate -l 256M $flash_path2}

start_server [list tags {"ext-storage-module-eviction"} overrides [list \
    ext-storage-enabled yes \
    maxmemory 8mb \
    maxmemory-policy allkeys-lru \
    hz 100 \
    loadmodule "$testmodule backend=flashcache db_path=$flash_path2 db_size_bytes=268435456" \
]] {

    test {Module: keys never deleted under memory pressure} {
        fill_memory
        wait_for_spill 50
        set dbsize [r dbsize]
        assert {$dbsize >= 9000}
    }

    test {Module: DBSIZE stays constant after sustained writes} {
        set before [r dbsize]
        set padding [string repeat "Y" 1024]
        for {set i 10000} {$i < 12000} {incr i} {
            catch {r set "extra:$i" "${padding}_$i"}
        }
        after 3000
        set after [r dbsize]
        assert {$after >= $before}
    }

    test {Module: server alive after eviction tests (no crash)} {
        assert_equal [r ping] PONG
    }
}

# ─── volatile-lru: tiering disabled ───────────────────────────────────────

set flash_path3 "/tmp/fc-module-test3-[pid].db"
catch {exec fallocate -l 256M $flash_path3}

start_server [list tags {"ext-storage-module-volatile"} overrides [list \
    ext-storage-enabled yes \
    maxmemory 8mb \
    maxmemory-policy volatile-lru \
    hz 100 \
    loadmodule "$testmodule backend=flashcache db_path=$flash_path3 db_size_bytes=268435456" \
]] {

    test {Module: volatile-lru disables tiering (0 spills)} {
        set padding [string repeat "W" 1024]
        for {set i 0} {$i < 5000} {incr i} {
            catch {r set "vol:$i" "${padding}_$i" EX 3600}
        }
        after 2000
        set info [r info all]
        if {[regexp {total_num_items_spilled_to_ext_storage:(\d+)} $info _ spilled]} {
            assert_equal $spilled 0
        }
        assert_equal [r ping] PONG
    }
}

# ─── noeviction: spilling works, no data loss ─────────────────────────────

set flash_path4 "/tmp/fc-module-test4-[pid].db"
catch {exec fallocate -l 256M $flash_path4}

start_server [list tags {"ext-storage-module-noevict"} overrides [list \
    ext-storage-enabled yes \
    maxmemory 8mb \
    maxmemory-policy noeviction \
    hz 100 \
    loadmodule "$testmodule backend=flashcache db_path=$flash_path4 db_size_bytes=268435456" \
]] {

    test {Module: noeviction allows spilling (data preserved)} {
        set padding [string repeat "N" 512]
        set written 0
        for {set i 0} {$i < 10000} {incr i} {
            if {[catch {r set "noev:$i" "${padding}_$i"} err] == 0} {
                incr written
            }
        }
        assert {$written > 100}
        set dbsize [r dbsize]
        assert_equal $dbsize $written
        assert_equal [r ping] PONG
    }
}

# ─── noeviction + flash full: OOM ────────────────────────────────────────

set flash_path5 "/tmp/fc-module-test5-[pid].db"
catch {exec fallocate -l 64M $flash_path5}

start_server [list tags {"ext-storage-module-oom"} overrides [list \
    ext-storage-enabled yes \
    maxmemory 4mb \
    maxmemory-policy noeviction \
    hz 100 \
    loadmodule "$testmodule backend=flashcache db_path=$flash_path5 db_size_bytes=67108864" \
]] {

    test {Module: noeviction + flash full returns OOM (no data loss)} {
        set padding [string repeat "F" 512]
        set written 0
        set oom_count 0
        for {set i 0} {$i < 20000} {incr i} {
            if {[catch {r set "full:$i" "${padding}_$i"} err] == 0} {
                incr written
            } else {
                incr oom_count
            }
        }
        assert {$written > 50}
        assert {$oom_count > 0}
        set dbsize [r dbsize]
        assert_equal $dbsize $written
        assert_equal [r ping] PONG
    }
}

# Cleanup
catch {file delete $flash_path}
catch {file delete $flash_path2}
catch {file delete $flash_path3}
catch {file delete $flash_path4}
catch {file delete $flash_path5}
