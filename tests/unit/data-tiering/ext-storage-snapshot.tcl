# Data tiering: RDB snapshotting with flash-resident values.
#
# Covers the fork-based snapshot design (V1):
#   - foreground SAVE materializes tiered values on the main thread while the
#     storage IO worker is parked
#   - BGSAVE forks after extStorageSnapshotPrepare() (drain -> hold -> GC
#     pause); the child reads flash synchronously via the fork-read path
#   - the produced RDB is 100% standard: tiered values are spliced from
#     their on-flash DUMP payloads, so any node can load it
#   - on load, previously-tiered values start in memory and re-tier under
#     memory pressure
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-snapshot

proc get_tiering_counter {name} {
    set info [r info all]
    if {[regexp "${name}:(\\d+)" $info _ val]} { return $val }
    return 0
}

proc wait_for_counter {name expected_min {timeout 10000}} {
    set start [clock milliseconds]
    while {1} {
        set now [get_tiering_counter $name]
        if {$now >= $expected_min} { return $now }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for $name >= $expected_min (stuck at $now)"
        }
        after 50
    }
}

proc debug_spill_wait {key {timeout 5000}} {
    set before [get_tiering_counter total_num_items_spilled_to_ext_storage]
    r debug spill $key
    set start [clock milliseconds]
    while {1} {
        set now [get_tiering_counter total_num_items_spilled_to_ext_storage]
        if {$now > $before} { return }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill of '$key'"
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
    set _path "/tmp/valkey-flash-snapshot-[pid].db"
}

start_server [list tags {"ext-storage" "ext-storage-snapshot"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path \
    ext-storage-capacity-mb 256 \
    maxmemory 10mb \
    maxmemory-policy allkeys-lru \
    appendonly no \
    save {} \
]] {

    test {snapshot INFO fields present and backend supports snapshotting} {
        set info [r info all]
        assert_match "*snapshot_supported:1*" $info
        assert_match "*snapshot_active:0*" $info
    }

    test {SAVE with all data types on flash round-trips via DEBUG RELOAD} {
        r flushall
        # One key of every type, all spilled
        r set snap_str "[string repeat s 500]"
        r hset snap_hash f1 v1 f2 v2 f3 [string repeat h 200]
        r rpush snap_list a b c [string repeat l 200]
        r sadd snap_set x y z [string repeat e 200]
        r zadd snap_zset 1 a 2 b 3 [string repeat z 200]
        r xadd snap_stream "*" field [string repeat x 200]
        # And one key that stays in memory
        r set snap_mem "memory_resident_value"

        foreach k {snap_str snap_hash snap_list snap_set snap_zset snap_stream} {
            debug_spill_wait $k
        }
        assert {[get_tiering_counter num_items_on_flash] >= 6}

        set saves_before [get_tiering_counter snapshot_saves]
        r debug reload
        assert {[get_tiering_counter snapshot_saves] > $saves_before}

        # Everything back, correct values
        assert_equal 500 [r strlen snap_str]
        assert_equal "v1" [r hget snap_hash f1]
        assert_equal 200 [string length [r hget snap_hash f3]]
        assert_equal 4 [r llen snap_list]
        assert_equal "a" [r lindex snap_list 0]
        assert_equal 4 [r scard snap_set]
        assert_equal 1 [r sismember snap_set x]
        assert_equal 3 [r zcard snap_zset]
        assert_equal 1 [r zscore snap_zset a]
        assert_equal 1 [r xlen snap_stream]
        assert_equal "memory_resident_value" [r get snap_mem]

        # Snapshot bookkeeping released
        assert_equal 0 [get_tiering_counter snapshot_active]
    }

    test {TTL survives snapshot of a flash-resident key} {
        r set snap_ttl "[string repeat t 300]" ex 200
        debug_spill_wait snap_ttl
        r debug reload
        assert_equal 300 [r strlen snap_ttl]
        set ttl [r ttl snap_ttl]
        assert {$ttl > 100 && $ttl <= 200}
    }

    test {BGSAVE with tiered values completes and RDB is loadable} {
        r flushall
        for {set i 0} {$i < 50} {incr i} {
            r set bulk:$i "[string repeat v 300]-$i"
        }
        for {set i 0} {$i < 25} {incr i} {
            debug_spill_wait bulk:$i
        }
        assert {[get_tiering_counter num_items_on_flash] >= 25}

        r bgsave
        waitForBgsave r
        assert_match "*rdb_last_bgsave_status:ok*" [r info persistence]
        # GC pause released after child reaped
        assert_equal 0 [get_tiering_counter snapshot_active]

        # Load the produced RDB and verify all 50 keys
        r debug reload
        assert_equal 50 [r dbsize]
        for {set i 0} {$i < 50} {incr i} {
            assert_equal "[string repeat v 300]-$i" [r get bulk:$i]
        }
    }

    test {ongoing writes during BGSAVE do not corrupt the snapshot} {
        r flushall
        for {set i 0} {$i < 30} {incr i} {
            r set base:$i "[string repeat b 400]"
        }
        for {set i 0} {$i < 15} {incr i} {
            debug_spill_wait base:$i
        }
        r config set rdb-key-save-delay 1000 ;# ~1ms per key: child lives ~30ms+
        r bgsave
        # Concurrent traffic while the child runs: overwrites, new keys,
        # deletes, and new spills
        for {set i 0} {$i < 30} {incr i} {
            r set during:$i "written-during-bgsave"
            r set base:$i "overwritten-during-bgsave-$i"
        }
        r del base:0
        waitForBgsave r
        r config set rdb-key-save-delay 0
        assert_match "*rdb_last_bgsave_status:ok*" [r info persistence]

        # Live dataset reflects the concurrent writes (snapshot is of the
        # fork instant; the live keyspace must be unaffected by it)
        assert_equal 0 [r exists base:0]
        assert_equal "overwritten-during-bgsave-5" [r get base:5]
        assert_equal "written-during-bgsave" [r get during:7]
    }

    test {tiering remains fully functional after snapshot (GC/hold released)} {
        r flushall
        r set post_snap "[string repeat p 300]"
        debug_spill_wait post_snap
        r debug reload
        # Spill again after the reload -- proves the IO worker and (for real
        # FC) the GC were unparked/unpaused
        r set post_snap2 "[string repeat q 300]"
        debug_spill_wait post_snap2
        assert_equal 300 [r strlen post_snap2]
    }

    test {snapshot prepare refuses when completions are stalled} {
        r flushall
        r set stall_key "[string repeat k 300]"
        debug_spill_wait stall_key
        # Freeze completion processing, then issue a fetch from a second
        # client (it blocks in KBC with the READ permanently in flight).
        # prepare's settle loop must give up and SAVE must fail loudly
        # rather than snapshot a state with in-flight IO.
        r debug ext-storage-pause-completions 1
        set rd [valkey_deferring_client]
        $rd get stall_key
        wait_for_blocked_client
        catch {r save} err
        # Unfreeze; the blocked GET completes normally.
        r debug ext-storage-pause-completions 0
        assert_equal "[string repeat k 300]" [$rd read]
        $rd close
        assert_match "*ERR*" $err
    }

    test {re-spill after reload under real memory pressure} {
        r flushall
        r config set maxmemory 3mb
        # ~2.5MB of values: forces spilling
        for {set i 0} {$i < 500} {incr i} {
            r set mem:$i "[string repeat m 5000]"
        }
        wait_for_counter total_num_items_spilled_to_ext_storage 1 15000
        set spilled_before [get_tiering_counter num_items_on_flash]
        assert {$spilled_before > 0}

        r debug reload
        assert_equal 500 [r dbsize]
        # Values loaded to RAM exceed maxmemory -> engine re-tiers
        wait_for_counter num_items_on_flash 1 15000
        # Spot-check data integrity post re-tiering
        assert_equal 5000 [r strlen mem:123]
        assert_equal 5000 [r strlen mem:456]
        r config set maxmemory 10mb
    }
}
