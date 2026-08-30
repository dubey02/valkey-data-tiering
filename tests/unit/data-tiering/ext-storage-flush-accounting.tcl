#
# Flush accounting and the repl-diskless-load swapdb downgrade
#
# Two defects, both about the store outliving the keys that referenced it.
#
# num_items_on_flash is only ever moved by storage completion callbacks: a spill
# completion increments it, a fetch/delete/evict completion decrements it. A
# flush removes flash residency for a whole database without producing any
# completion at all, so the gauge used to stay phantom-high forever. That reaches
# past INFO: every save path gates its tiering branch on num_items_on_flash > 0,
# so a stale value makes a save arm a snapshot cut over an effectively empty
# store. The accounting now happens in emptyData(), which also covers the flush
# paths that are not commands (DEBUG RELOAD, DEBUG FLUSHALL, and the replica's
# flush-before-load full sync).
#
# repl-diskless-load swapdb has no temp external store to swap into: the flash
# section is ingested straight into the one process-global store. An aborted load
# frees only the in-memory temp DB, orphaning every record already ingested. It
# is therefore downgraded to flush-before-load at startup and refused at runtime.
#
# Runs against the mock by default. For real FlashCache:
#   EXT_STORAGE_BACKEND=flashcache ./runtest --single unit/data-tiering/ext-storage-flush-accounting

proc fa_counter {name} {
    set info [r info all]
    if {[regexp "${name}:(\\d+)" $info _ val]} { return $val }
    return 0
}

proc fa_spill_wait {key {timeout 5000}} {
    set before [fa_counter total_num_items_spilled_to_ext_storage]
    r debug spill $key
    set start [clock milliseconds]
    while {1} {
        if {[fa_counter total_num_items_spilled_to_ext_storage] > $before} { return }
        if {[clock milliseconds] - $start > $timeout} { error "Timed out spilling '$key'" }
        after 50
    }
}

if {[info exists ::env(EXT_STORAGE_BACKEND)]} {
    set _backend $::env(EXT_STORAGE_BACKEND)
} else {
    set _backend "flashcache-mock"
}
set _path "/tmp/fc-test-flushacct-[pid].db"
catch {exec fallocate -l 512M $_path}

start_server [list tags {"ext-storage" "ext-storage-flush-accounting"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path \
    ext-storage-capacity-mb 512 \
    maxmemory 200mb \
    maxmemory-policy allkeys-lru \
    appendonly no \
    save {} \
]] {

    # --- FLUSHALL -----------------------------------------------------------

    test {flush accounting: FLUSHALL returns num_items_on_flash to zero} {
        r flushall
        r select 0
        for {set i 0} {$i < 20} {incr i} {
            r set fa:all:$i [string repeat A 4096]
            fa_spill_wait fa:all:$i
        }
        assert_equal 20 [fa_counter num_items_on_flash]

        r flushall
        # Synchronous: emptyData drains and wipes before returning, so there is
        # no completion still in flight that could move the gauge afterwards.
        assert_equal 0 [fa_counter num_items_on_flash]
    }

    test {flush accounting: FLUSHALL ASYNC returns num_items_on_flash to zero} {
        r flushall
        for {set i 0} {$i < 10} {incr i} {
            r set fa:async:$i [string repeat B 4096]
            fa_spill_wait fa:async:$i
        }
        assert_equal 10 [fa_counter num_items_on_flash]

        # ASYNC defers freeing the keyspace to the lazyfree thread, but the store
        # flush and the gauge are still settled on the main thread before return.
        r flushall async
        assert_equal 0 [fa_counter num_items_on_flash]
    }

    # --- FLUSHDB ------------------------------------------------------------
    #
    # The single-db case cannot just zero the gauge: the other databases' records
    # are still live. It subtracts only the flushed db's tiered entries, which is
    # why the accounting has to run before the keyspace is emptied.

    test {flush accounting: FLUSHDB subtracts only the flushed database} {
        r flushall
        r select 0
        for {set i 0} {$i < 12} {incr i} {
            r set fa:db0:$i [string repeat C 4096]
            fa_spill_wait fa:db0:$i
        }
        r select 5
        for {set i 0} {$i < 7} {incr i} {
            r set fa:db5:$i [string repeat D 4096]
            fa_spill_wait fa:db5:$i
        }
        assert_equal 19 [fa_counter num_items_on_flash]

        # Flush db 5 only: db 0's twelve records must survive the accounting.
        r select 5
        r flushdb
        assert_equal 12 [fa_counter num_items_on_flash]

        # And db 0's keys are genuinely still readable from flash.
        r select 0
        assert_equal 4096 [string length [r get fa:db0:0]]

        r select 0
        r flushall
        assert_equal 0 [fa_counter num_items_on_flash]
    }

    test {flush accounting: FLUSHDB on an untiered database leaves the gauge alone} {
        r flushall
        r select 0
        for {set i 0} {$i < 8} {incr i} {
            r set fa:keep:$i [string repeat E 4096]
            fa_spill_wait fa:keep:$i
        }
        assert_equal 8 [fa_counter num_items_on_flash]

        # db 7 holds only in-memory keys, so nothing should be subtracted.
        r select 7
        r set fa:mem:1 small
        r flushdb
        assert_equal 8 [fa_counter num_items_on_flash]

        r select 0
        r flushall
    }

    # --- non-command flush paths -------------------------------------------
    #
    # These are the paths the old placement in flushdbCommand/flushallCommand
    # missed entirely. DEBUG RELOAD stands in for them here because it reaches
    # emptyData() the same way a replica's flush-before-load full sync does.

    test {flush accounting: DEBUG RELOAD leaves no phantom flash residency} {
        r flushall
        r select 0
        for {set i 0} {$i < 15} {incr i} {
            r set fa:reload:$i [string repeat F 4096]
            fa_spill_wait fa:reload:$i
        }
        assert_equal 15 [fa_counter num_items_on_flash]

        # The save materializes the tiered values into the RDB; the load empties
        # the keyspace (and now the store) and brings them back in memory. Either
        # way the gauge must describe reality rather than the pre-reload world.
        r debug reload
        assert_equal 15 [r dbsize]

        set on_flash [fa_counter num_items_on_flash]
        assert {$on_flash <= 15}
        # Values must still be intact whatever tier they came back on.
        assert_equal 4096 [string length [r get fa:reload:0]]
        assert_equal 4096 [string length [r get fa:reload:14]]

        r flushall
        assert_equal 0 [fa_counter num_items_on_flash]
    }

    test {flush accounting: a save after FLUSHALL does not arm a tiering cut} {
        r flushall
        r select 0
        for {set i 0} {$i < 20} {incr i} {
            r set fa:cut:$i [string repeat G 4096]
            fa_spill_wait fa:cut:$i
        }
        set saves_before [fa_counter snapshot_stream_saves]

        r flushall
        assert_equal 0 [fa_counter num_items_on_flash]

        # With the gauge honest, rdb.c's `num_items_on_flash > 0` gate is false
        # and the save skips the streaming path. Before the fix the stale gauge
        # armed a cut over an empty store on every save.
        r save
        assert_equal $saves_before [fa_counter snapshot_stream_saves]
    }

    # --- P3: runtime CONFIG SET --------------------------------------------

    test {swapdb downgrade: CONFIG SET repl-diskless-load swapdb is refused} {
        set before [lindex [r config get repl-diskless-load] 1]
        assert_error "*not supported while data tiering*" {
            r config set repl-diskless-load swapdb
        }
        # Rejection must roll the value back, not leave it half-applied.
        assert_equal $before [lindex [r config get repl-diskless-load] 1]
    }

    test {swapdb downgrade: the other repl-diskless-load values still apply} {
        foreach v {disabled on-empty-db flush-before-load} {
            r config set repl-diskless-load $v
            assert_equal $v [lindex [r config get repl-diskless-load] 1]
        }
    }
}

catch {exec rm -f $_path}

# --- P3: startup downgrade -------------------------------------------------
#
# A config file naming swapdb must come up on flush-before-load rather than
# either refusing to start or silently running with the unsafe setting.

set _path2 "/tmp/fc-test-flushacct-swap-[pid].db"
catch {exec fallocate -l 256M $_path2}

start_server [list tags {"ext-storage" "ext-storage-flush-accounting"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path2 \
    ext-storage-capacity-mb 256 \
    maxmemory 200mb \
    maxmemory-policy allkeys-lru \
    repl-diskless-load swapdb \
    appendonly no \
    save {} \
]] {
    test {swapdb downgrade: startup with tiering rewrites swapdb to flush-before-load} {
        assert_equal "flush-before-load" [lindex [r config get repl-diskless-load] 1]
    }

    test {swapdb downgrade: startup logs why it changed the setting} {
        verify_log_message 0 "*Downgrading to flush-before-load*" 0
    }
}

catch {exec rm -f $_path2}

# --- control: no tiering, no downgrade -------------------------------------

start_server [list tags {"ext-storage" "ext-storage-flush-accounting"} overrides [list \
    repl-diskless-load swapdb \
    appendonly no \
    save {} \
]] {
    test {swapdb downgrade: untiered server keeps swapdb} {
        assert_equal "swapdb" [lindex [r config get repl-diskless-load] 1]
    }
}
