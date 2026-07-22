# Data tiering: DEL/UNLINK semantics for flash-resident keys.
#
# Regression suite for the "DEL of a flash key replies 0" bug. The optimized
# delete path (no fetch) used to remove the entry inside the flash-delete
# completion, so the re-executed DEL found nothing: wrong reply (0), no
# signalModifiedKey (WATCH not invalidated), no keyspace "del" notification,
# no dirty++. Fixed internal-TS style: the completion keeps the entry in
# TIERING_STATE_PENDING_DELETION and the re-executed DEL performs the real
# keyspace removal with full command-layer side effects.
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-del-semantics

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

# Non-embedded value so DEBUG SPILL accepts it
proc big_val {} { return [string repeat z 300] }

start_server [list tags {"ext-storage" "ext-storage-del-semantics"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache-mock \
    ext-storage-path "/tmp/valkey-flash-del-[pid].db" \
    ext-storage-capacity-mb 256 \
    maxmemory 50mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command yes \
]] {

    test {DEL of flash-resident key returns 1} {
        r set delkey [big_val]
        debug_spill_wait delkey
        assert_equal 1 [r del delkey]
        assert_equal 0 [r exists delkey]
    }

    test {UNLINK of flash-resident key returns 1} {
        r set unlinkkey [big_val]
        debug_spill_wait unlinkkey
        assert_equal 1 [r unlink unlinkkey]
        assert_equal 0 [r exists unlinkkey]
    }

    test {DEL of mixed keys counts flash, memory, and absent correctly} {
        r set flashkey [big_val]
        debug_spill_wait flashkey
        r set memkey inmemory
        # flashkey (on flash) + memkey (in memory) + nosuchkey (absent) => 2
        assert_equal 2 [r del flashkey memkey nosuchkey]
        assert_equal 0 [r exists flashkey]
        assert_equal 0 [r exists memkey]
    }

    test {DEL of two flash-resident keys returns 2} {
        r set fk1 [big_val]
        r set fk2 [big_val]
        debug_spill_wait fk1
        debug_spill_wait fk2
        assert_equal 2 [r del fk1 fk2]
    }

    test {DEL of flash-resident key fires keyspace del notification} {
        r config set notify-keyspace-events KEA
        r set notifkey [big_val]
        debug_spill_wait notifkey
        set rd [valkey_deferring_client]
        $rd psubscribe "__keyevent@*__:del"
        assert_equal {psubscribe __keyevent@*__:del 1} [$rd read]
        assert_equal 1 [r del notifkey]
        set msg [$rd read]
        assert_equal {pmessage} [lindex $msg 0]
        assert_match {__keyevent@*__:del} [lindex $msg 2]
        assert_equal {notifkey} [lindex $msg 3]
        $rd close
        r config set notify-keyspace-events ""
    }

    test {DEL of flash-resident key invalidates WATCH} {
        r set watchkey [big_val]
        debug_spill_wait watchkey
        # Second connection WATCHes the key and opens a MULTI
        set rd [valkey_deferring_client]
        $rd watch watchkey
        assert_equal {OK} [$rd read]
        $rd multi
        assert_equal {OK} [$rd read]
        $rd get watchkey
        assert_equal {QUEUED} [$rd read]
        # Main connection deletes the flash-resident key -> must touch WATCH
        assert_equal 1 [r del watchkey]
        # EXEC must abort (nil) because the watched key was modified
        $rd exec
        assert_equal {} [$rd read]
        $rd close
    }

    test {GET queued behind pending DEL sees the key as deleted} {
        # GET arriving while the DEL is draining must block, then observe
        # the post-delete keyspace (nil), never the placeholder.
        r set racekey [big_val]
        debug_spill_wait racekey
        set rd [valkey_deferring_client]
        # Fire DEL and GET back-to-back on separate connections; DEL blocks
        # on the flash key, GET queues behind PENDING_DELETION.
        $rd del racekey
        set rd2 [valkey_deferring_client]
        $rd2 get racekey
        assert_equal 1 [$rd read]
        assert_equal {} [$rd2 read]
        $rd close
        $rd2 close
    }

    test {SET after DEL of flash-resident key creates a fresh key} {
        r set cyclekey [big_val]
        debug_spill_wait cyclekey
        assert_equal 1 [r del cyclekey]
        r set cyclekey freshvalue
        assert_equal {freshvalue} [r get cyclekey]
    }

    test {dirty counter increments for flash-resident DEL} {
        r set dirtykey [big_val]
        debug_spill_wait dirtykey
        set before [s rdb_changes_since_last_save]
        assert_equal 1 [r del dirtykey]
        assert {[s rdb_changes_since_last_save] > $before}
    }
}
