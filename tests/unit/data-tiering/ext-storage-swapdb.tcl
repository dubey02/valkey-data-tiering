# Data tiering: SWAPDB support and runtime config guards.
#
# SWAPDB: flash records and in-flight IO are addressed by a PHYSICAL db id
# (ext_storage.c db-id indirection). SWAPDB swaps the logical->physical
# mapping together with the keyspaces, so flash-resident values stay
# reachable and completions route to the keyspace that owns them.
# (Before this: flash data was stranded under the pre-swap id -- fetch miss,
# and pre-miss-fix an infinite KBC resubmit wedge.)
#
# Policy guard: CONFIG SET maxmemory-policy to volatile-*/allkeys-random is
# rejected while tiering is active (spill victim selection supports only
# allkeys-lru, allkeys-lfu, noeviction). The same rule at init disables
# tiering; the runtime path used to bypass it silently.
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-swapdb

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

proc debug_spill_wait {key} {
    set before [get_tiering_counter total_num_items_spilled_to_ext_storage]
    r debug spill $key
    wait_for_counter total_num_items_spilled_to_ext_storage [expr {$before + 1}]
}

proc bigval {tag} { return "${tag}[string repeat z 300]" }

# Real FlashCache asserts on backing-file size inside getFileSize() before its
# logger exists, so a missing or undersized file segfaults during init with no
# usable message (the harness only reports "Can't start / No PID detected").
# Pre-allocating removes that entirely. Harmless for the mock backend.
set _fcpath "/tmp/valkey-flash-swap-[pid].db"
catch {exec fallocate -l 256M $_fcpath}

start_server [list tags {"ext-storage" "ext-storage-swapdb"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache-mock \
    ext-storage-path $_fcpath \
    ext-storage-capacity-mb 256 \
    maxmemory 50mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command yes \
]] {

    test {SWAPDB: flash-resident keys in both dbs stay reachable after swap} {
        r select 0
        r set k0 [bigval A]
        debug_spill_wait k0
        r select 1
        r set k1 [bigval B]
        debug_spill_wait k1
        r select 9 ;# framework default db; swap 0<->1 from here
        r swapdb 0 1
        r select 1
        assert_match {A*} [r getrange k0 0 3]
        r select 0
        assert_match {B*} [r getrange k1 0 3]
        r select 9
        assert_equal 0 [get_tiering_counter completion_read_miss]
    }

    test {SWAPDB: swap back and re-fetch} {
        r swapdb 0 1
        r select 0
        assert_equal 301 [r strlen k0]
        r select 1
        assert_equal 301 [r strlen k1]
        r select 9
        assert_equal 0 [get_tiering_counter completion_read_miss]
    }

    test {SWAPDB: DEL of a flash-resident key after swap works with correct reply} {
        r select 0
        r set delk [bigval D]
        debug_spill_wait delk
        r select 9
        r swapdb 0 1
        r select 1
        assert_equal 1 [r del delk]
        assert_equal 0 [r exists delk]
        r select 9
    }

    test {SWAPDB: new spills after swap land in the right namespace} {
        r select 1
        r set postswap [bigval P]
        debug_spill_wait postswap
        assert_equal 301 [r strlen postswap]
        # swap again and read from the other side
        r select 9
        r swapdb 0 1
        r select 0
        assert_match {P*} [r getrange postswap 0 3]
        r select 9
        assert_equal 0 [get_tiering_counter completion_read_miss]
    }

    test {SWAPDB: same-index swap is a no-op that works} {
        r select 0
        r set samek [bigval S]
        debug_spill_wait samek
        r select 9
        r swapdb 0 0
        r select 0
        assert_equal 301 [r strlen samek]
        r select 9
    }

    test {POLICY GUARD: volatile-* and allkeys-random rejected at runtime with tiering active} {
        foreach p {volatile-lru volatile-lfu volatile-random volatile-ttl allkeys-random} {
            assert_error "*must be allkeys-lru, allkeys-lfu, or noeviction*" {r config set maxmemory-policy $p}
        }
        # value must be unchanged after rejected sets
        assert_equal {allkeys-lru} [lindex [r config get maxmemory-policy] 1]
    }

    test {POLICY GUARD: supported policies still settable at runtime} {
        assert_equal {OK} [r config set maxmemory-policy allkeys-lfu]
        assert_equal {OK} [r config set maxmemory-policy noeviction]
        assert_equal {OK} [r config set maxmemory-policy allkeys-lru]
    }

    test {POLICY GUARD: tiering still functional after rejected policy attempts} {
        r set guardk [bigval G]
        debug_spill_wait guardk
        assert_equal 301 [r strlen guardk]
    }

    test {PENDING-DEL GUARD: SWAPDB rejected while a DEL of a flash-resident key is in flight} {
        r select 0
        r set pdkey [bigval P]
        debug_spill_wait pdkey
        r select 9
        # Hold completions so the DEL stays blocked-in-use deterministically
        r debug ext-storage-pause-completions 1
        set rd [valkey_deferring_client]
        $rd select 0
        assert_equal {OK} [$rd read]
        $rd del pdkey
        # Ensure the DEL command reached the server and blocked before SWAPDB
        wait_for_blocked_client
        # SWAPDB touching the DEL's db must be rejected while the DEL drains
        assert_error "*SWAPDB unable to complete*" {r swapdb 0 1}
        assert_error "*SWAPDB unable to complete*" {r swapdb 1 0}
        # SWAPDB on unrelated dbs is unaffected
        assert_equal {OK} [r swapdb 2 3]
        # Release completions: DEL drains with the correct reply
        r debug ext-storage-pause-completions 0
        assert_equal 1 [$rd read]
        $rd close
        # Retry now succeeds, and the deleted key is gone in both views
        assert_equal {OK} [r swapdb 0 1]
        r select 1
        assert_equal 0 [r exists pdkey]
        r select 9
        r swapdb 0 1 ;# restore
    }

    test {PENDING-DEL GUARD: blocked non-DEL client does not trip the guard} {
        r select 0
        r set fetchkey [bigval F]
        debug_spill_wait fetchkey
        r select 9
        r debug ext-storage-pause-completions 1
        set rd [valkey_deferring_client]
        $rd select 0
        assert_equal {OK} [$rd read]
        # GET blocks in-use on the flash fetch, but it is not a DEL — SWAPDB
        # must proceed (db-id indirection routes the fetch correctly).
        $rd get fetchkey
        wait_for_blocked_client
        assert_equal {OK} [r swapdb 0 1]
        r debug ext-storage-pause-completions 0
        # The GET re-executes against its logical db0, which post-swap no
        # longer holds the key -> nil (the GET linearizes after SWAPDB).
        assert_equal {} [$rd read]
        $rd close
        # The fetched value landed in the keyspace that owns it (now db1)
        r select 1
        assert_equal 301 [r strlen fetchkey]
        r select 9
        r swapdb 0 1 ;# restore
    }
}

# Guard must NOT fire when tiering is disabled
start_server [list tags {"ext-storage" "ext-storage-swapdb"} overrides [list \
    ext-storage-enabled no \
]] {
    test {POLICY GUARD: volatile-* allowed when tiering is disabled} {
        assert_equal {OK} [r config set maxmemory-policy volatile-lru]
        assert_equal {OK} [r config set maxmemory-policy noeviction]
    }
}
