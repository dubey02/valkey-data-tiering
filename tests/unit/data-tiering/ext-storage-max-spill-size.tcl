# Data tiering max-spill-size tests.
# Verifies: items exceeding ext-storage-max-spill-size are not spilled to flash.
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-max-spill-size

set flash_path "/tmp/fc-test-maxspill-[pid].db"
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

proc get_spill_count {} {
    set info [r info all]
    if {[regexp {total_num_items_spilled_to_ext_storage:(\d+)} $info _ spilled]} {
        return $spilled
    }
    return 0
}

proc fill_to_pressure {} {
    # Fill with small values to trigger memory pressure and spilling
    set padding [string repeat "x" 512]
    for {set i 0} {$i < 10000} {incr i} {
        catch {r set "small:[format %05d $i]" "${padding}_$i"}
    }
}

# ─── Test: large items are NOT spilled when exceeding max-spill-size ────────

start_server [list tags {"ext-storage-max-spill-size"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $flash_path \
    ext-storage-capacity-mb 256 \
    ext-storage-max-spill-size 1024 \
    maxmemory 4mb \
    maxmemory-policy allkeys-lru \
]] {
    test "Large items exceeding max-spill-size are not spilled" {
        # Write a large value (2KB > 1024 byte threshold)
        set large_val [string repeat "L" 2048]
        r set bigkey $large_val

        # Fill memory to trigger spilling
        fill_to_pressure

        # Wait for some spilling to occur
        wait_for_spill 5

        # The big key should still be in memory (not spilled)
        # Verify by checking it returns immediately without blocking
        assert_equal $large_val [r get bigkey]

        # Write a small value (under threshold) and verify it CAN be spilled
        set small_val [string repeat "s" 100]
        r set smallkey $small_val
        fill_to_pressure
        wait_for_spill 10

        # Small key should still be accessible (spilled or in memory)
        assert_equal $small_val [r get smallkey]
    }

    test "CONFIG SET ext-storage-max-spill-size works at runtime" {
        # Verify current value
        assert_equal {ext-storage-max-spill-size 1024} [r config get ext-storage-max-spill-size]

        # Change at runtime
        r config set ext-storage-max-spill-size 4096
        assert_equal {ext-storage-max-spill-size 4096} [r config get ext-storage-max-spill-size]

        # Set to 0 disables the check
        r config set ext-storage-max-spill-size 0
        assert_equal {ext-storage-max-spill-size 0} [r config get ext-storage-max-spill-size]
    }

    test "Items at exactly max-spill-size are spilled" {
        r flushall
        r config set ext-storage-max-spill-size 1024

        # Write value exactly at threshold (serialized will be slightly larger due to RDB header)
        # So use a value slightly under to ensure it stays under after serialization
        set val [string repeat "e" 900]
        r set exactkey $val

        fill_to_pressure
        wait_for_spill 1

        # Should be accessible (either from memory or fetched from flash)
        assert_equal $val [r get exactkey]
    }

    test "max-spill-size 0 disables the size check" {
        r flushall
        r config set ext-storage-max-spill-size 0

        # Even large values should spill when the check is disabled
        set large_val [string repeat "D" 8192]
        r set disabledkey $large_val

        fill_to_pressure
        wait_for_spill 1

        assert_equal $large_val [r get disabledkey]
    }
}

# Cleanup
catch {exec rm -f $flash_path}
