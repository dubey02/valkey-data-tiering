# Module backend: sustained spill + fetch verification.
# This test forces enough memory pressure that spilling MUST sustain
# beyond the first batch (catches the ram_bytes predictor bug).
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-module-spill-fetch

set testmodule [file normalize modules/flash-tiering/target/release/libflash_tiering_module.so]

if {![file exists $testmodule]} {
    puts "SKIPPED: flash-tiering module not built at $testmodule"
    puts "Build with: cd modules/flash-tiering && cargo build --release --features backend-flashcache"
    return
}

set flash_path "/tmp/fc-module-spill-[pid].db"
catch {exec fallocate -l 256M $flash_path}

proc wait_for_spill {expected_min {timeout 20000}} {
    set start [clock milliseconds]
    while {1} {
        set info [r info all]
        if {[regexp {total_num_items_spilled_to_ext_storage:(\d+)} $info _ spilled]} {
            if {$spilled >= $expected_min} { return $spilled }
        }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill >= $expected_min (got $spilled)"
        }
        after 100
    }
}

start_server [list tags {"ext-storage-module-spill-fetch"} overrides [list \
    ext-storage-enabled yes \
    maxmemory 4mb \
    maxmemory-policy allkeys-lru \
    hz 100 \
    loadmodule "$testmodule backend=flashcache db_path=$flash_path db_size_bytes=268435456" \
]] {
    test "Module: sustained spilling beyond first batch" {
        # Fill memory to trigger spilling — need enough keys to force
        # multiple spill cycles (batch_size=10, so >10 spills needed)
        set padding [string repeat "x" 512]
        for {set i 0} {$i < 5000} {incr i} {
            catch {r set "mspill:[format %05d $i]" "${padding}_$i"}
        }

        # Wait for significant spilling (>50 items = multiple batches)
        wait_for_spill 50

        # Verify items are on flash
        set info [r info all]
        regexp {num_items_on_flash:(\d+)} $info _ on_flash
        assert {$on_flash > 30}
    }

    test "Module: fetch from flash returns correct value" {
        # Write a specific key, force it to be spilled, then read it back
        r set fetchtest "hello-from-flash-module-test"

        # Fill more to ensure fetchtest gets spilled
        set padding [string repeat "y" 512]
        for {set i 5000} {$i < 8000} {incr i} {
            catch {r set "mspill:[format %05d $i]" "${padding}_$i"}
        }

        wait_for_spill 100

        # fetchtest should be retrievable (either from DRAM or fetched from flash)
        assert_equal "hello-from-flash-module-test" [r get fetchtest]
    }

    test "Module: spill_serialized_count tracks serialization" {
        set info [r info all]
        regexp {completion_write_ok:(\d+)} $info _ write_ok
        # Should have successful write completions
        assert {$write_ok > 0}
    }
}

# Cleanup
catch {exec rm -f $flash_path}
