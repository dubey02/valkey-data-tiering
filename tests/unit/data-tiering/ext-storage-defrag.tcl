# Data tiering + active defrag integration tests.
# Verifies defrag doesn't crash when keys are in various tiering states.
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-defrag

set flash_path "/tmp/fc-test-defrag-[pid].db"
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

start_server [list tags {"ext-storage-defrag"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $flash_path \
    ext-storage-capacity-mb 256 \
    maxmemory 8mb \
    maxmemory-policy allkeys-lru \
    hz 100 \
    activedefrag yes \
    active-defrag-ignore-bytes 1mb \
    active-defrag-threshold-lower 0 \
    active-defrag-cycle-min 99 \
    active-defrag-cycle-max 99 \
]] {

    test {Defrag: server starts with activedefrag + tiering} {
        assert_equal [r ping] PONG
        set defrag [r config get activedefrag]
        assert_equal [lindex $defrag 1] yes
    }

    test {Defrag: runs without crash during active spilling} {
        # Fill memory to trigger spilling — defrag runs concurrently
        fill_memory
        wait_for_spill 50
        # Let defrag run for a few cycles while spilling is active
        after 3000
        assert_equal [r ping] PONG
    }

    test {Defrag: data integrity after defrag + spilling} {
        # Verify some keys are still readable
        set val [r get "filler:00001"]
        assert {$val ne {}}
        assert {[string length $val] > 100}
    }

    test {Defrag: config active and no crash after defrag cycle time} {
        # Verify defrag is configured (actual defrag depends on jemalloc version)
        set info [r config get activedefrag]
        assert_equal [lindex $info 1] yes
        # Give defrag time to attempt a cycle
        after 1000
        assert_equal [r ping] PONG
    }

    test {Defrag: no crash with sustained writes + defrag + spilling} {
        # Continuous writes while defrag is running — stress test
        set padding [string repeat "Z" 512]
        for {set i 0} {$i < 5000} {incr i} {
            catch {r set "stress:$i" "${padding}_$i"}
        }
        after 2000
        assert_equal [r ping] PONG
        # Keys survive
        set dbsize [r dbsize]
        assert {$dbsize > 5000}
    }

    test {Defrag: FLUSHDB + defrag doesn't crash} {
        r flushdb
        after 1000
        fill_memory
        wait_for_spill 10
        # Defrag running while keys are tiered
        after 2000
        r flushdb
        after 500
        assert_equal [r dbsize] 0
        assert_equal [r ping] PONG
    }

    test {Defrag: server alive after all defrag tests (no crash)} {
        assert_equal [r ping] PONG
    }
}

# Cleanup
catch {file delete $flash_path}
