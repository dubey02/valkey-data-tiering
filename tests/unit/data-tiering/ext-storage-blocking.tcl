# Data tiering: blocking commands (BLPOP/BLMOVE/BLMPOP/XREAD BLOCK) with
# flash-resident keys, plus RDB fail-loudly and DEBUG OBJECT behavior.
#
# Blocking commands have two layers of blocking with tiering: the KBC tiering
# block (fetch from flash) happens FIRST via preCommandExec, then the command
# executes and may enter its own list/stream blocking. These tests cover the
# interaction of the two layers.
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-blocking

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

proc bigelem {tag} { return "${tag}[string repeat z 300]" }

start_server [list tags {"ext-storage" "ext-storage-blocking"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache-mock \
    ext-storage-path "/tmp/valkey-flash-blk-[pid].db" \
    ext-storage-capacity-mb 256 \
    maxmemory 50mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command yes \
]] {

    test {BLPOP on flash-resident list pops immediately after fetch} {
        r del blist
        r rpush blist [bigelem A] [bigelem B]
        debug_spill_wait blist
        set res [r blpop blist 5]
        assert_equal {blist} [lindex $res 0]
        assert_match {A*} [lindex $res 1]
        assert_equal 1 [r llen blist]
    }

    test {BLMOVE with flash-resident source} {
        r del bsrc bdst
        r rpush bsrc [bigelem M1] [bigelem M2]
        debug_spill_wait bsrc
        set v [r blmove bsrc bdst left right 5]
        assert_match {M1*} $v
        assert_equal 1 [r llen bdst]
        assert_equal 1 [r llen bsrc]
    }

    test {BLMPOP with flash-resident list} {
        r del bmp
        r rpush bmp [bigelem P1] [bigelem P2]
        debug_spill_wait bmp
        set res [r blmpop 5 1 bmp left]
        assert_equal {bmp} [lindex $res 0]
        assert_match {P1*} [lindex [lindex $res 1] 0]
    }

    test {BLPOP blocks on missing key and is woken by RPUSH} {
        r del wakekey
        set rd [valkey_deferring_client]
        $rd blpop wakekey 10
        wait_for_blocked_client
        r rpush wakekey woken
        assert_equal {wakekey woken} [$rd read]
        $rd close
    }

    test {XREAD BLOCK woken by XADD to a stream that spilled while blocked} {
        r del bstream
        r xadd bstream * f [bigelem S]
        # Reader blocks waiting for entries NEWER than current last-id
        set rd [valkey_deferring_client]
        $rd xread block 10000 streams bstream $
        wait_for_blocked_client
        # Stream spills while the reader is blocked
        debug_spill_wait bstream
        # XADD from another client: KBC fetches the stream back, appends,
        # signals the blocked reader
        r xadd bstream * f2 newentry
        set res [$rd read]
        assert_match {*newentry*} $res
        $rd close
        assert_equal {PONG} [r ping]
    }

    # ------------------------------------------------------------------
    # RDB snapshotting with tiered values (full coverage in
    # ext-storage-snapshot.tcl; these assert the paths formerly gated)
    # ------------------------------------------------------------------

    test {SAVE succeeds while values are on flash} {
        r set rdbgate "[string repeat q 300]"
        debug_spill_wait rdbgate
        assert {[get_tiering_counter num_items_on_flash] > 0}
        assert_equal {OK} [r save]
        assert_match {Background saving started*} [r bgsave]
        waitForBgsave r
        # Value untouched, still flash-resident or fetchable
        assert_equal 300 [r strlen rdbgate]
    }

    test {DEBUG RELOAD round-trips flash values} {
        r set rdbgate2 "[string repeat w 300]"
        debug_spill_wait rdbgate2
        assert {[get_tiering_counter num_items_on_flash] > 0}
        r debug reload
        # After reload, values that were on flash are back (in memory)
        assert_equal 300 [r strlen rdbgate]
        assert_equal 300 [r strlen rdbgate2]
    }

    test {SWAPDB works with tiering (db-id indirection)} {
        # Full coverage in ext-storage-swapdb.tcl; this asserts the command
        # is no longer gated (it was, before the indirection landed).
        assert_equal {OK} [r swapdb 0 1]
        r swapdb 0 1 ;# restore
    }

    test {DEBUG OBJECT on flash-resident key reports tiering info without crashing} {
        r set dbgkey "[string repeat q 300]"
        debug_spill_wait dbgkey
        set res [r debug object dbgkey]
        assert_match {*encoding:tiered*} $res
        assert_match {*value_on_external_storage:1*} $res
        assert_equal {PONG} [r ping]
        # After a fetch, DEBUG OBJECT returns normal info again
        r getrange dbgkey 0 0
        set res [r debug object dbgkey]
        assert_match {*serializedlength*} $res
    }
}
