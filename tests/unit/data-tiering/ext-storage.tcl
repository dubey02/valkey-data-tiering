# Non-key-spilling data tiering integration tests.
#
# These tests require the key-spilling module to be built:
#   cd modules/key-spilling && cargo build --release
#
# Run with:
#   ./runtest --single tests/unit/ext-storage.tcl

set testmodule [file normalize modules/key-spilling/target/release/libkey_spilling_module.so]

# Skip if module not built
if {![file exists $testmodule]} {
    puts "SKIPPED: key-spilling module not built at $testmodule"
    puts "Build with: cd modules/key-spilling && cargo build --release"
    return
}

proc wait_for_spill {expected_min {timeout 5000}} {
    # Wait until at least expected_min items have been spilled
    set start [clock milliseconds]
    while {1} {
        set info [r info all]
        if {[regexp {total_num_items_spilled_to_ext_storage:(\d+)} $info _ spilled]} {
            if {$spilled >= $expected_min} {
                return $spilled
            }
        }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill (got $spilled, wanted $expected_min)"
        }
        after 50
    }
}

proc get_info_field {field} {
    set info [r info all]
    if {[regexp "${field}:(\\S+)" $info _ val]} {
        return $val
    }
    return ""
}

start_server [list tags {"ext-storage"} overrides [list \
    ext-storage-enabled yes \
    maxmemory 2mb \
    maxmemory-policy allkeys-lru \
    loadmodule "$testmodule backend=rocksdb db_path=/tmp/valkey-test-tiering-[pid]" \
]] {

    test {Non-key-spilling: keys remain in dict after spill} {
        # Fill memory well past maxmemory to trigger spilling.
        # With 2MB maxmemory, spill threshold is 2.2MB. Base memory is ~1MB.
        # Use 1KB values × 2000 keys = 2MB of values → total ~3MB → exceeds threshold.
        set padding [string repeat "ABCDEFGHIJKLMNOP" 64]
        for {set i 0} {$i < 2000} {incr i} {
            r set "key:[format %06d $i]" "v[format %06d $i]_${padding}"
        }

        # Wait for some items to be spilled
        set spilled [wait_for_spill 50 30000]
        assert {$spilled >= 50}

        # DBSIZE should still show all keys (non-key-spilling: keys stay in dict)
        set dbsize [r dbsize]
        assert_equal $dbsize 2000
    }

    test {Non-key-spilling: EXISTS returns 1 for tiered keys} {
        # All keys should exist regardless of whether value is in memory or on disk
        set exists_count 0
        for {set i 0} {$i < 100} {incr i} {
            if {[r exists "key:[format %06d $i]"]} {
                incr exists_count
            }
        }
        assert_equal $exists_count 100
    }

    test {Non-key-spilling: GET fetches value from disk} {
        # Access a key that's likely tiered (old LRU). The value should be
        # fetched transparently.
        set val [r get "key:000000"]
        assert {[string match "v000000_*" $val]}
    }

    test {Non-key-spilling: TTL preserved across spill/fetch} {
        # Set a key with TTL — it will be spilled since memory is already over threshold
        # Use catch since we might hit OOM; the key might already exist from initial fill
        r set "key:000050" "ttl_value_with_padding_to_exceed_embstr" EX 3600
        after 2000
        # TTL should still be approximately 3600
        set ttl [r ttl "key:000050"]
        assert {$ttl > 3500 && $ttl <= 3600}
    }

    test {Non-key-spilling: TYPE works on tiered keys} {
        set t [r type "key:000100"]
        assert_equal $t "string"
    }

    test {Non-key-spilling: SCAN includes tiered keys} {
        set all_keys {}
        set cursor 0
        while {1} {
            set result [r scan $cursor COUNT 1000]
            set cursor [lindex $result 0]
            set keys [lindex $result 1]
            foreach k $keys {
                lappend all_keys $k
            }
            if {$cursor == 0} break
        }
        set total [llength $all_keys]
        assert {$total >= 2000}
    }

    test {Non-key-spilling: DEL works on tiered keys} {
        r del "key:000000"
        assert_equal [r exists "key:000000"] 0
    }

    test {Non-key-spilling: SET overwrites tiered key} {
        r set "key:000001" "new_value_padded_to_avoid_embstr_encoding_threshold"
        set val [r get "key:000001"]
        assert_equal $val "new_value_padded_to_avoid_embstr_encoding_threshold"
    }

    test {Non-key-spilling: OBJECT ENCODING on tiered key} {
        r get "key:000002"
        set enc [r object encoding "key:000002"]
        assert {$enc eq "raw" || $enc eq "embstr"}
    }

    test {Non-key-spilling: INFO shows spill/fetch metrics} {
        set spilled [get_info_field "total_num_items_spilled_to_ext_storage"]
        set fetched [get_info_field "total_num_items_fetched_from_ext_storage"]
        assert {$spilled > 0}
        assert {$fetched > 0}
    }

    test {Non-key-spilling: MGET fetches multiple tiered keys} {
        set vals [r mget "key:000010" "key:000011" "key:000012"]
        foreach v $vals {
            assert {[string match "v0000*" $v]}
        }
    }

    test {Non-key-spilling: EXPIRE on tiered key works} {
        r expire "key:001999" 7200
        set ttl [r ttl "key:001999"]
        assert {$ttl > 7100 && $ttl <= 7200}
    }
}
