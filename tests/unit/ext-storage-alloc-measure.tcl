# Allocation measurement test for embstr spill tombstone.
# Measures actual jemalloc allocation footprint of a dbEntry before and after
# spill, to determine if the tombstone is genuinely smaller than the original.
#
# Run with:
#   ./runtest --single unit/ext-storage-alloc-measure

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

proc get_debug_object_field {key field} {
    set dbg [r debug object $key]
    if {[regexp "${field}:(\\S+)" $dbg _ val]} {
        return $val
    }
    return ""
}

start_server [list tags {"ext-storage"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache-mock \
    maxmemory 50mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command local \
]] {

    test {ALLOC-MEASURE: single key embstr entry vs tiered tombstone footprint} {
        r flushall
        after 100

        # Create one embstr key: ~15-byte key, ~37-byte value, with TTL
        set key "alloc_measure_k1"
        set val "abcdefghijklmnopqrstuvwxyz0123456789A"
        r set $key $val PX 3600000

        assert_equal [r object encoding $key] "embstr"

        # A = original embstr entry allocation (single allocation, value embedded)
        set A [get_debug_object_field $key "ext_entry_usable"]
        puts ">>> A (original embstr entry usable): $A bytes"
        assert {$A > 0}

        # Spill the key
        set spill_before [get_info_field "total_num_items_spilled_to_ext_storage"]
        r debug spill $key
        wait_for_spill_count [expr {$spill_before + 1}]

        # B = tombstone robj allocation, C = tombstone empty-sds allocation
        set B [get_debug_object_field $key "ext_entry_usable"]
        set C [get_debug_object_field $key "ext_val_usable"]
        puts ">>> B (tombstone entry usable): $B bytes"
        puts ">>> C (tombstone val_sds usable): $C bytes"
        set BC [expr {$B + $C}]
        puts ">>> B+C = $BC vs A = $A"
        if {$BC < $A} {
            puts ">>> RESULT: Tombstone IS smaller (saves [expr {$A - $BC}] bytes/key)"
        } else {
            puts ">>> RESULT: Tombstone is NOT smaller (LARGER by [expr {$BC - $A}] bytes/key)"
        }
        # Sanity: both should be > 0
        assert {$B > 0}
        assert {$C > 0}
    }

    test {ALLOC-MEASURE: used_memory per-key delta over 200 keys} {
        r flushall
        after 200

        set val "abcdefghijklmnopqrstuvwxyz0123456789A"
        set num_keys 200

        # Settle memory
        set mem0 [get_info_field "used_memory"]

        # Create keys
        for {set i 0} {$i < $num_keys} {incr i} {
            r set "mkey:[format %05d $i]_pad" $val PX 3600000
        }
        set mem_after_set [get_info_field "used_memory"]

        # Spill all
        set spill_before [get_info_field "total_num_items_spilled_to_ext_storage"]
        for {set i 0} {$i < $num_keys} {incr i} {
            r debug spill "mkey:[format %05d $i]_pad"
        }
        wait_for_spill_count [expr {$spill_before + $num_keys}] 15000

        after 200
        set mem_after_spill [get_info_field "used_memory"]

        set delta_total [expr {$mem_after_spill - $mem0}]
        set delta_per_key [expr {$delta_total / $num_keys}]
        set spill_delta [expr {$mem_after_spill - $mem_after_set}]
        set spill_per_key [expr {$spill_delta / $num_keys}]

        puts ">>> used_memory baseline: $mem0"
        puts ">>> used_memory after SET $num_keys keys: $mem_after_set"
        puts ">>> used_memory after SPILL all: $mem_after_spill"
        puts ">>> Total delta (spill - baseline): $delta_total ($delta_per_key bytes/key)"
        puts ">>> Spill delta (after_spill - after_set): $spill_delta ($spill_per_key bytes/key)"
        if {$spill_per_key < 0} {
            puts ">>> RESULT: Memory DECREASED by [expr {abs($spill_per_key)}] bytes/key after spill (tombstone reclaims)"
        } else {
            puts ">>> RESULT: Memory INCREASED by $spill_per_key bytes/key after spill"
        }
    }
}
