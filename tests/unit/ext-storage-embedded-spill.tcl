# Phase 1: Embedded string spill gate correctness tests.
#
# Uses the flashcache-mock backend (default when ext-storage-enabled=yes).
#
# LIMITATION: The native flashcache-mock backend does not serialize/deserialize
# robj values (it stores 0-byte blobs), so full round-trip (GET after spill)
# cannot be validated locally. Only the GATE logic and completion transition are
# tested here. Full round-trip is validated by the remote r7gd test harness
# using the real FlashCache backend.
#
# Run with:
#   ./runtest --single unit/ext-storage-embedded-spill

proc get_info_field {field} {
    set info [r info all]
    if {[regexp "${field}:(\\S+)" $info _ val]} {
        return $val
    }
    return ""
}

proc wait_for_spill_count {expected_min {timeout 5000}} {
    set start [clock milliseconds]
    while {1} {
        set spilled [get_info_field "total_num_items_spilled_to_ext_storage"]
        if {$spilled >= $expected_min} {
            return $spilled
        }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill count >= $expected_min (got $spilled)"
        }
        after 50
    }
}

if {[info exists ::env(EXT_STORAGE_BACKEND)]} {
    set _backend $::env(EXT_STORAGE_BACKEND)
} else {
    set _backend "flashcache-mock"
}
if {[info exists ::env(EXT_STORAGE_PATH)]} {
    set _path $::env(EXT_STORAGE_PATH)
} else {
    set _path "/tmp/valkey-flash-embspill-[pid].db"
}

start_server [list tags {"ext-storage"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path \
    ext-storage-capacity-mb 256 \
    maxmemory 50mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command local \
]] {

    test {Embstr string passes spill gate and completion transitions to TIERED} {
        # Create an embstr-encoded value (~37 bytes, fits in embstr threshold).
        set key "embstr_test_key_pad20"
        set val "abcdefghijklmnopqrstuvwxyz0123456789A"
        r set $key $val PX 3600000

        # Confirm it starts as embstr
        assert_equal [r object encoding $key] "embstr"

        # Force spill via DEBUG SPILL (bypasses automatic sampling, tests serialize path)
        r debug spill $key

        # Wait for the write completion to fire
        wait_for_spill_count 1

        # The completion handler should have transitioned the entry.
        # Use DEBUG OBJECT to check encoding without triggering a fetch.
        # (OBJECT ENCODING triggers lookupKeyRead which blocks and fetches on ONLY_FLASH).
        # Instead, check INFO stats which confirm the spill completed successfully.
        set write_ok [get_info_field "completion_write_ok"]
        assert {$write_ok >= 1}

        set spilled [get_info_field "total_num_items_spilled_to_ext_storage"]
        assert {$spilled >= 1}

        # NOTE: Full round-trip GET correctness (value equality + TTL preservation)
        # cannot be tested with the flashcache-mock backend because it stores 0-byte
        # blobs (it doesn't serialize robj). Validated on remote with real FlashCache.
        # Memory-reduction assertion is deferred to Phase 2.
    }

    test {Phase 2: embstr spill reclaims memory (tombstone smaller than original)} {
        # With the flashcache-mock, the mock backend itself stores data in
        # zmalloc, so used_memory doesn't decrease absolutely. Instead, we
        # verify the per-key INCREASE is bounded (tombstone is smaller than
        # original embstr, so increase = mock_overhead - reclaimed_bytes).
        # Without reclamation, increase would be larger (leak + no shrink).
        r flushall
        after 100

        set num_keys 500
        set val "abcdefghijklmnopqrstuvwxyz0123456789A"
        for {set i 0} {$i < $num_keys} {incr i} {
            r set "emb:[format %05d $i]_pad20" $val PX 3600000
        }

        set spill_before [get_info_field "total_num_items_spilled_to_ext_storage"]
        set mem_before [get_info_field "used_memory"]

        for {set i 0} {$i < $num_keys} {incr i} {
            r debug spill "emb:[format %05d $i]_pad20"
        }

        set target [expr {$spill_before + $num_keys}]
        wait_for_spill_count $target 15000

        set mem_after [get_info_field "used_memory"]
        set increase [expr {$mem_after - $mem_before}]
        # NOTE: aggregate used_memory / keys conflates mock-backend bookkeeping
        # with the tombstone swap. This repo's flashcache-mock copies the full
        # key+value bytes per entry (fcEntry + key copy + value copy ~= 112 B/key),
        # unlike the private repo's 0-byte-blob mock the original <100 threshold
        # was calibrated against.
        # Measured here: WITH reclamation ~104 B/key (mock 112 - tombstone saving);
        # WITHOUT reclamation: ~128 B/key (104 + 24 B/key not reclaimed, from the
        # 80 B entry -> 48+8 B tombstone delta measured by alloc-measure).
        # Threshold 120 sits between and still discriminates. The exact tombstone
        # footprint (80 B entry -> 48+8 B) is asserted directly by
        # ext-storage-alloc-measure.tcl via per-key allocation probes.
        set per_key [expr {$increase / $num_keys}]
        assert {$per_key < 120}
        # Also verify all completions fired
        set write_ok [get_info_field "completion_write_ok"]
        assert {$write_ok >= $target}
    }

    test {Phase 2: repeated spill cycles do not leak memory} {
        # Verify no per-key leak by checking that the per-key memory increase
        # from spilling is consistent across cycles (stable, not growing).
        r flushall
        after 100
        set num_keys 300
        set val "abcdefghijklmnopqrstuvwxyz0123456789A"

        # Cycle 1
        set spill_before [get_info_field "total_num_items_spilled_to_ext_storage"]
        for {set i 0} {$i < $num_keys} {incr i} {
            r set "cyc1:[format %05d $i]_pad" $val PX 3600000
        }
        set mem_pre1 [get_info_field "used_memory"]
        for {set i 0} {$i < $num_keys} {incr i} {
            r debug spill "cyc1:[format %05d $i]_pad"
        }
        wait_for_spill_count [expr {$spill_before + $num_keys}] 15000
        set mem_post1 [get_info_field "used_memory"]
        set delta1 [expr {$mem_post1 - $mem_pre1}]

        # Cycle 2 (no flushall — keys accumulate in mock, but we only care about delta)
        set spill_before2 [get_info_field "total_num_items_spilled_to_ext_storage"]
        for {set i 0} {$i < $num_keys} {incr i} {
            r set "cyc2:[format %05d $i]_pad" $val PX 3600000
        }
        set mem_pre2 [get_info_field "used_memory"]
        for {set i 0} {$i < $num_keys} {incr i} {
            r debug spill "cyc2:[format %05d $i]_pad"
        }
        wait_for_spill_count [expr {$spill_before2 + $num_keys}] 15000
        set mem_post2 [get_info_field "used_memory"]
        set delta2 [expr {$mem_post2 - $mem_pre2}]

        # Per-key spill cost should be stable across cycles (no leak).
        # Allow 20 bytes/key variance for allocator jitter.
        set diff [expr {abs($delta2 - $delta1)}]
        set per_key_diff [expr {$diff / $num_keys}]
        assert {$per_key_diff < 20}
    }

    test {INT-encoded string is NOT spilled by automatic path} {
        # Create an INT-encoded value early so it has old LRU
        r config set maxmemory 2mb
        r set int_key 12345
        assert_equal [r object encoding int_key] "int"

        # Fill memory to trigger automatic spilling with large raw strings.
        set padding [string repeat "X" 1024]
        for {set i 0} {$i < 2000} {incr i} {
            r set "filler:[format %04d $i]" "${padding}_${i}"
        }

        # Wait for some automatic spills to happen (filler keys should spill)
        wait_for_spill_count 10 15000

        # The int key must NOT have been spilled — encoding should still be "int".
        # Using OBJECT ENCODING is safe here: if the key is NOT tiered, no fetch occurs.
        assert_equal [r object encoding int_key] "int"
        assert_equal [r get int_key] "12345"
    }
}
