# Write-through of client writes to flash-resident keys under transient
# promotion policies (promotion=never / 2hit-50k first hit).
#
# Regression tests for the lost-write bug: a SET on an ONLY_FLASH key used to
# return +OK but the transient revert in extStorageFreeTransientValues()
# discarded the new value and reinstalled the tombstone, so subsequent reads
# fetched the OLD value from flash.
#
# BACKEND: real FlashCache (not flashcache-mock) — these tests validate FC's
# log-index supersede semantics (new value served from the staging buffer
# before flush, from the new on-disk location after flush, old entry GC'd),
# which the mock does not model. The backing file is provisioned by the test.
#
# Run with:
#   ./runtest --single tests/unit/ext-storage-write-through.tcl

proc wt_info_field {field} {
    set info [r info all]
    if {[regexp "${field}:(\\S+)" $info _ val]} {
        return $val
    }
    return ""
}

proc wt_wait_spill_idle {{timeout 5000}} {
    set start [clock milliseconds]
    while {1} {
        if {[wt_info_field "num_items_spilling_to_ext_storage"] == 0} return
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill pipeline to drain"
        }
        after 10
    }
}

proc wt_spill_key {key} {
    r debug spill $key
    wt_wait_spill_idle
}

# Provision the FlashCache backing file (flashcacheInit asserts file >= capacity)
set wt_fc_path "/tmp/valkey-test-wt-[pid].db"
exec fallocate -l 300M $wt_fc_path

start_server [list tags {"ext-storage"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $wt_fc_path \
    ext-storage-capacity-mb 256 \
    ext-storage-promotion-policy never \
    maxmemory 64mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command yes \
]] {
    set v1 [string repeat A 300]
    set v2 [string repeat B 300]
    set v3 [string repeat C 300]

    test {WT-1: SET on flash-resident key persists (supersede in staging)} {
        r set wt:k1 $v1
        wt_spill_key wt:k1
        assert_equal $v1 [r get wt:k1]   ;# transient serve of old value works
        r set wt:k1 $v2                  ;# overwrite while flash-resident
        wt_wait_spill_idle               ;# write-through spill completes
        assert {[wt_info_field "transient_write_throughs"] >= 1}
        assert_equal $v2 [r get wt:k1]   ;# fetch must see the NEW value
    }

    test {WT-2: superseded value survives a staging-buffer flush} {
        # Push >1MB (buffered-write-flush-threshold) of filler through the
        # spill path so wt:k1's superseding entry is flushed to disk, then
        # verify the fetch still resolves to the new value (on-disk index).
        set filler [string repeat F 102400]
        for {set i 0} {$i < 15} {incr i} {
            r set wt:filler:$i $filler
            wt_spill_key wt:filler:$i
        }
        assert_equal $v2 [r get wt:k1]
    }

    test {WT-3: in-place modification (APPEND) on flash-resident key persists} {
        r set wt:k2 $v1
        wt_spill_key wt:k2
        r append wt:k2 "-suffix"
        wt_wait_spill_idle
        assert_equal "${v1}-suffix" [r get wt:k2]
    }

    test {WT-4: last write wins for multiple SETs on a flash-resident key} {
        r set wt:k3 $v1
        wt_spill_key wt:k3
        r set wt:k3 $v2
        r set wt:k3 $v3
        wt_wait_spill_idle
        assert_equal $v3 [r get wt:k3]
    }

    test {WT-5: DEL of flash-resident key does not resurrect old value} {
        r set wt:k4 $v1
        wt_spill_key wt:k4
        r del wt:k4
        assert_equal 0 [r exists wt:k4]
        assert_equal "" [r get wt:k4]
        # Re-create with a new value: fetch path must never see the old copy
        r set wt:k4 $v2
        wt_spill_key wt:k4
        assert_equal $v2 [r get wt:k4]
    }

    test {WT-6: clean (read-only) transients still revert — never stays never} {
        r set wt:k5 $v1
        wt_spill_key wt:k5
        set fetches_before [wt_info_field "completion_read_ok"]
        assert_equal $v1 [r get wt:k5]
        assert_equal $v1 [r get wt:k5]
        set fetches_after [wt_info_field "completion_read_ok"]
        # Both GETs fetched from flash: the value was NOT retained in DRAM
        assert {$fetches_after >= $fetches_before + 2}
        assert_equal 0 [wt_info_field "transient_write_through_retained"]
    }
}

start_server [list tags {"ext-storage"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $wt_fc_path \
    ext-storage-capacity-mb 256 \
    ext-storage-promotion-policy 2hit-50k \
    maxmemory 64mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command yes \
]] {
    set v1 [string repeat A 300]
    set v2 [string repeat B 300]

    test {WT-7: 2hit-50k first-hit overwrite persists (transient path)} {
        r set wt2:k1 $v1
        wt_spill_key wt2:k1
        # First access is the transient (1st-hit) path; overwrite during it
        r set wt2:k1 $v2
        wt_wait_spill_idle
        assert {[wt_info_field "transient_write_throughs"] >= 1}
        assert_equal $v2 [r get wt2:k1]
    }
}

exec rm -f $wt_fc_path
