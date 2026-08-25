# Warm retention under promotion=never (ext-storage-warm-retention).
#
# WARM = value resident in RAM AND valid clean copy on flash. Reads are dict
# hits (no fetch); a client write flips the key dirty (ONLY_MEMORY) and the
# next spill supersedes; deleting a WARM key deletes the flash copy; pressure
# demotes WARM keys for free (tombstone, no IO) before spilling dirty keys.
#
# BACKEND: real FlashCache — validates PEEK preservation and supersede.
#
# Run with:
#   ./runtest --single tests/unit/ext-storage-warm.tcl

proc warm_info_field {field} {
    set info [r info all]
    if {[regexp "${field}:(\\S+)" $info _ val]} {
        return $val
    }
    return ""
}

proc warm_wait_spill_idle {{timeout 5000}} {
    set start [clock milliseconds]
    while {1} {
        if {[warm_info_field "num_items_spilling_to_ext_storage"] == 0} return
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill pipeline to drain"
        }
        after 10
    }
}

proc warm_spill_key {key} {
    r debug spill $key
    warm_wait_spill_idle
}

set warm_fc_path "/tmp/valkey-test-warm-[pid].db"
exec fallocate -l 300M $warm_fc_path

start_server [list tags {"ext-storage"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $warm_fc_path \
    ext-storage-capacity-mb 256 \
    ext-storage-promotion-policy never \
    ext-storage-warm-retention yes \
    maxmemory 64mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command yes \
]] {
    set v1 [string repeat A 300]
    set v2 [string repeat B 300]

    test {WARM-1: spill retains the value as a clean warm resident} {
        r set warm:k1 $v1
        set reads_before [warm_info_field "completion_read_ok"]
        warm_spill_key warm:k1
        assert {[warm_info_field "warm_installs_spill"] >= 1}
        assert {[warm_info_field "warm_keys_resident"] >= 1}
        assert_equal $v1 [r get warm:k1]
        # The read was a dict hit — no flash fetch happened
        assert_equal $reads_before [warm_info_field "completion_read_ok"]
        assert {[warm_info_field "warm_hits"] >= 1}
    }

    test {WARM-2: fetch installs the value as warm; repeat reads skip the disk} {
        # Build a tombstoned key: retention off so the spill frees the value
        r config set ext-storage-warm-retention no
        r set warm:k2 $v1
        warm_spill_key warm:k2
        r config set ext-storage-warm-retention yes

        set reads_before [warm_info_field "completion_read_ok"]
        assert_equal $v1 [r get warm:k2]     ;# fetch from flash → warm install
        assert_equal [expr {$reads_before + 1}] [warm_info_field "completion_read_ok"]
        assert {[warm_info_field "warm_installs_fetch"] >= 1}
        assert_equal $v1 [r get warm:k2]     ;# warm hit — NO second fetch
        assert_equal $v1 [r get warm:k2]     ;# still no fetch
        assert_equal [expr {$reads_before + 1}] [warm_info_field "completion_read_ok"]
    }

    test {WARM-3: a write flips warm to dirty and the respill supersedes} {
        set flips_before [warm_info_field "warm_dirty_flips"]
        r set warm:k1 $v2                    ;# overwrite the warm resident
        assert {[warm_info_field "warm_dirty_flips"] > $flips_before}
        assert_equal $v2 [r get warm:k1]     ;# RAM copy is authoritative
        # Respill with retention off so the entry tombstones, then prove the
        # fetch resolves to the NEW value (supersede reached flash).
        r config set ext-storage-warm-retention no
        warm_spill_key warm:k1
        assert_equal $v2 [r get warm:k1]
        r config set ext-storage-warm-retention yes
    }

    test {WARM-4: DEL of a warm key deletes the flash copy — no resurrection} {
        r set warm:k4 $v1
        warm_spill_key warm:k4               ;# retained as WARM
        set dels_before [warm_info_field "warm_flash_dels"]
        r del warm:k4
        assert_equal 0 [r exists warm:k4]
        assert {[warm_info_field "warm_flash_dels"] > $dels_before}
        # Recreate with a new value; the fetch path must never see the old copy
        r set warm:k4 $v2
        warm_spill_key warm:k4
        assert_equal $v2 [r get warm:k4]
    }

    test {WARM-5: pressure demotes warm keys and memory stays bounded} {
        # Populate a batch of warm residents, then shrink maxmemory below
        # current usage so the next spill-controller pass must reclaim.
        set filler [string repeat F 102400]
        for {set i 0} {$i < 50} {incr i} {
            r set warm:fill:$i $filler
            warm_spill_key warm:fill:$i
        }
        assert {[warm_info_field "warm_keys_resident"] >= 50}
        set used [status r used_memory]
        r config set maxmemory [expr {$used - (20 * 102400)}]
        # Trigger controller passes with light traffic
        for {set i 0} {$i < 100} {incr i} { r set warm:poke $i }
        wait_for_condition 100 50 {
            [warm_info_field "warm_demotions"] >= 1
        } else {
            fail "No warm demotions under memory pressure"
        }
        r config set maxmemory 64mb
        # Demoted keys still readable (fetch from flash)
        assert_equal $filler [r get warm:fill:0]
    }
}

# Default-off regression guard: without warm retention, promotion=never keeps
# today's transient semantics — every read of a flash key fetches.
start_server [list tags {"ext-storage"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $warm_fc_path \
    ext-storage-capacity-mb 256 \
    ext-storage-promotion-policy never \
    maxmemory 64mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command yes \
]] {
    set v1 [string repeat A 300]

    test {WARM-6: retention off (default) — transient serve, every read fetches} {
        r set warm:off:k1 $v1
        r debug spill warm:off:k1
        warm_wait_spill_idle
        set reads_before [warm_info_field "completion_read_ok"]
        assert_equal $v1 [r get warm:off:k1]
        assert_equal $v1 [r get warm:off:k1]
        assert {[warm_info_field "completion_read_ok"] >= $reads_before + 2}
        assert_equal 0 [warm_info_field "warm_keys_resident"]
    }
}

exec rm -f $warm_fc_path
