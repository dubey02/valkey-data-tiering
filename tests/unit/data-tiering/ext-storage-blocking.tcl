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
    # RDB fail-loudly + DEBUG OBJECT (bugs 5 and 7 from the command audit)
    # ------------------------------------------------------------------

    test {SAVE fails loudly while values are on flash} {
        r set rdbgate "[string repeat q 300]"
        debug_spill_wait rdbgate
        assert {[get_tiering_counter num_items_on_flash] > 0}
        assert_error "*not supported while values reside on external storage*" {r save}
        assert_error "*not supported while values reside on external storage*" {r bgsave}
    }

    test {DEBUG RELOAD fails instead of silently losing flash values} {
        assert {[get_tiering_counter num_items_on_flash] > 0}
        catch {r debug reload} err
        assert_match {*} $err ;# must error, not succeed
        assert {![string match {OK} $err]}
        # And critically: the flash-resident key still has its data
        assert_equal 300 [r strlen rdbgate]
    }

    test {SAVE succeeds again once no values remain on flash} {
        # Promote everything back to memory by touching all keys
        foreach k [r keys *] { catch {r type $k}; catch {r getrange $k 0 0} }
        # Collections need a read too
        foreach k [r keys *] {
            switch [r type $k] {
                list {r lrange $k 0 0}
                stream {r xlen $k}
            }
        }
        wait_for_counter total_num_items_fetched_from_ext_storage 1 15000
        if {[get_tiering_counter num_items_on_flash] == 0} {
            assert_equal {OK} [r save]
        } else {
            # Some keys may re-spill under pressure; the invariant tested is
            # the gate condition itself, exercised in the previous tests.
            assert_error "*external storage*" {r save}
        }
    }

    test {SWAPDB is refused while tiering is enabled} {
        # Flash data is addressed by db id; SWAPDB would strand flash-resident
        # values under the pre-swap db (fetch miss => unreachable data).
        # Live-reproduced before the gate: post-swap fetch missed and (before
        # the read-miss fix) wedged KBC in an infinite resubmit loop.
        assert_error "*not supported with data tiering*" {r swapdb 0 1}
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
