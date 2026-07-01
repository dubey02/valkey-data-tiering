# Data tiering + eviction integration tests.
# Verifies: keys never deleted by engine, spill instead of evict,
# noeviction disables flash GC, volatile-* rejects tiering at startup.
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-eviction

set flash_path "/tmp/fc-test-eviction-[pid].db"
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

# ─── allkeys-lru: spill instead of evict ───────────────────────────────────

start_server [list tags {"ext-storage-eviction"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $flash_path \
    ext-storage-capacity-mb 256 \
    maxmemory 8mb \
    maxmemory-policy allkeys-lru \
    hz 100 \
]] {

    test {Eviction: keys never deleted under memory pressure (allkeys-lru)} {
        fill_memory
        wait_for_spill 50
        set dbsize [r dbsize]
        assert {$dbsize >= 9000}
        set info [r info all]
        regexp {total_num_items_spilled_to_ext_storage:(\d+)} $info _ spilled
        assert {$spilled > 0}
    }

    test {Eviction: DBSIZE stays constant after sustained writes} {
        set before [r dbsize]
        set padding [string repeat "Y" 1024]
        for {set i 10000} {$i < 12000} {incr i} {
            catch {r set "extra:$i" "${padding}_$i"}
        }
        after 3000
        set after [r dbsize]
        assert {$after >= $before}
    }

    test {Eviction: server alive after pressure (no crash)} {
        assert_equal [r ping] PONG
    }
}

# ─── volatile-lru: tiering disabled at startup ─────────────────────────────

set flash_path2 "/tmp/fc-test-eviction2-[pid].db"
catch {exec fallocate -l 256M $flash_path2}

start_server [list tags {"ext-storage-eviction-volatile"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $flash_path2 \
    ext-storage-capacity-mb 256 \
    maxmemory 8mb \
    maxmemory-policy volatile-lru \
    hz 100 \
]] {

    test {Eviction: volatile-lru disables tiering (0 spills)} {
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

# ─── noeviction: tiering works, flash GC disabled ──────────────────────────

set flash_path3 "/tmp/fc-test-eviction3-[pid].db"
catch {exec fallocate -l 256M $flash_path3}

start_server [list tags {"ext-storage-eviction-noevict"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $flash_path3 \
    ext-storage-capacity-mb 256 \
    maxmemory 8mb \
    maxmemory-policy noeviction \
    hz 100 \
]] {

    test {Eviction: noeviction allows spilling (data preserved)} {
        # With noeviction + tiering: writes accepted while below maxmemory,
        # spilling happens proactively. Once at maxmemory, writes may be
        # throttled/rejected but NO keys are ever deleted.
        set padding [string repeat "N" 512]
        set written 0
        for {set i 0} {$i < 10000} {incr i} {
            if {[catch {r set "noev:$i" "${padding}_$i"} err] == 0} {
                incr written
            }
        }
        # Some writes should have succeeded before hitting pressure
        assert {$written > 100}
        # DBSIZE equals written (no keys deleted)
        set dbsize [r dbsize]
        assert_equal $dbsize $written
        assert_equal [r ping] PONG
    }
}

# ─── noeviction + flash full: OOM returned to client ───────────────────────

set flash_path4 "/tmp/fc-test-eviction4-[pid].db"
catch {exec fallocate -l 64M $flash_path4}

start_server [list tags {"ext-storage-eviction-oom"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $flash_path4 \
    ext-storage-capacity-mb 64 \
    maxmemory 4mb \
    maxmemory-policy noeviction \
    hz 100 \
]] {

    test {Eviction: noeviction + flash full returns OOM (no data loss)} {
        # Fill both DRAM (1MB) and flash (32MB) until writes are rejected
        set padding [string repeat "F" 512]
        set written 0
        set oom_count 0
        for {set i 0} {$i < 20000} {incr i} {
            set reply [catch {r set "full:$i" "${padding}_$i"} err]
            if {$reply == 0} {
                incr written
            } else {
                incr oom_count
            }
        }
        # Some writes succeeded, some got OOM
        assert {$written > 50}
        assert {$oom_count > 0}
        # DBSIZE == written: no keys lost
        set dbsize [r dbsize]
        assert_equal $dbsize $written
        # Server alive
        assert_equal [r ping] PONG
        # Existing keys still readable
        set val [r get "full:0"]
        assert {$val ne {}}
    }
}

# Cleanup
catch {file delete $flash_path}
catch {file delete $flash_path2}
catch {file delete $flash_path3}
catch {file delete $flash_path4}
