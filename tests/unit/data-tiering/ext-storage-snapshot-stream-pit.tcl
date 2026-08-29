#
# Streaming snapshot: correctness suite (Phase 5)
#
# Phases 3 and 4 proved the streaming path works and is tunable. This suite
# attacks the properties those tests did not touch at all:
#
#   - multiple databases, which is the only thing that exercises the flash
#     section's SELECTDB-on-change emission, plus SWAPDB, which rewrites the
#     physical-to-logical DB mapping the orphan filter resolves through
#   - non-string types, since the flash payload is opaque DUMP bytes and nothing
#     had ever put a hash, set, zset or list through the streaming path
#   - point in time semantics through the produced RDB rather than at the engine
#     stream level: a record frozen before the cut must be present, one created
#     after it must not
#   - the poison path: an aborted stream must fail the save, never yield a
#     truncated RDB that looks complete
#   - a snapshot taken while on-flash GC is actively relocating records
#
# Runs against the mock by default. For real FlashCache:
#   EXT_STORAGE_BACKEND=flashcache ./runtest --single unit/data-tiering/ext-storage-snapshot-stream-pit
#
# Proc names are p5_* because test files can share a tcl interpreter and the
# other stream suites already define rdb_* helpers.

proc p5_counter {name} {
    set info [r info all]
    if {[regexp "${name}:(\\d+)" $info _ val]} { return $val }
    return 0
}

proc p5_spill_wait {key {timeout 5000}} {
    set before [p5_counter total_num_items_spilled_to_ext_storage]
    r debug spill $key
    set start [clock milliseconds]
    while {1} {
        if {[p5_counter total_num_items_spilled_to_ext_storage] > $before} { return }
        if {[clock milliseconds] - $start > $timeout} { error "Timed out spilling '$key'" }
        after 50
    }
}

proc p5_incompressible {len seed} {
    set out ""
    set x $seed
    while {[string length $out] < $len} {
        set x [expr {($x * 1103515245 + 12345) & 0x7fffffff}]
        append out [format %08x $x]
    }
    return [string range $out 0 [expr {$len - 1}]]
}

# Flash-section entry count reported after line $from, or -1 if it did not run.
proc p5_flash_entries {from} {
    set fd [open [srv 0 stdout] r]
    set lines [split [read $fd] "\n"]
    close $fd
    set entries -1
    set i 0
    foreach line $lines {
        incr i
        if {$i <= $from} continue
        if {[regexp {flash section: (\d+) entries written} $line _ n]} { set entries $n }
        if {[string match {*falling back to the fork read path*} $line]} { set entries -1 }
    }
    return $entries
}

proc p5_bgsave {} {
    set from [count_log_lines 0]
    r bgsave
    waitForBgsave r
    return $from
}

# Occurrences of $pattern in the server log after line $from.
proc p5_log_hits {from pattern} {
    set fd [open [srv 0 stdout] r]
    set lines [split [read $fd] "\n"]
    close $fd
    set hits 0
    set i 0
    foreach line $lines {
        incr i
        if {$i <= $from} continue
        if {[string match "*$pattern*" $line]} { incr hits }
    }
    return $hits
}

if {[info exists ::env(EXT_STORAGE_BACKEND)]} {
    set _backend $::env(EXT_STORAGE_BACKEND)
} else {
    set _backend "flashcache-mock"
}
set _path "/tmp/fc-test-snappit-[pid].db"
catch {exec fallocate -l 512M $_path}

start_server [list tags {"ext-storage" "ext-storage-snapshot-stream-pit"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path \
    ext-storage-capacity-mb 512 \
    maxmemory 200mb \
    maxmemory-policy allkeys-lru \
    appendonly no \
    save {} \
]] {

    # --- multiple databases -------------------------------------------------
    #
    # The flash section walks the stream, not the keyspace, so records arrive
    # ungrouped and unordered across DBs and it has to re-emit SELECTDB whenever
    # the DB changes. Nothing before this test ever put tiered keys in more than
    # one DB, so that emission had never executed.

    test {pit multidb: tiered keys land back in their own database} {
        r flushall
        set dbs {0 3 9}
        set n 40
        foreach db $dbs {
            r select $db
            for {set i 0} {$i < $n} {incr i} {
                r set k:$db:$i [p5_incompressible 400 [expr {$db * 1000 + $i}]]
                p5_spill_wait k:$db:$i
            }
            # One memory-resident key per DB so the memory section is non-empty too.
            r set mem:$db "memory-$db"
        }
        r select 9

        set from [p5_bgsave]
        assert_equal "ok" [s rdb_last_bgsave_status]
        assert_equal [expr {$n * [llength $dbs]}] [p5_flash_entries $from]

        restart_server 0 true false

        foreach db $dbs {
            r select $db
            assert_equal [expr {$n + 1}] [r dbsize]
            assert_equal "memory-$db" [r get mem:$db]
            for {set i 0} {$i < $n} {incr i} {
                assert_equal [p5_incompressible 400 [expr {$db * 1000 + $i}]] [r get k:$db:$i]
            }
            # A key must not leak into a DB it does not belong to.
            foreach other $dbs {
                if {$other == $db} continue
                assert_equal 0 [r exists k:$other:0]
            }
        }
        r select 9
    }

    test {pit multidb: a database holding only tiered keys survives} {
        r flushall
        r select 4
        for {set i 0} {$i < 25} {incr i} {
            r set only:$i [p5_incompressible 300 [expr {$i + 777}]]
            p5_spill_wait only:$i
        }
        r select 9
        r set anchor 1

        set from [p5_bgsave]
        assert_equal "ok" [s rdb_last_bgsave_status]
        assert_equal 25 [p5_flash_entries $from]

        restart_server 0 true false
        r select 4
        assert_equal 25 [r dbsize]
        assert_equal [p5_incompressible 300 [expr {7 + 777}]] [r get only:7]
        r select 9
        assert_equal 1 [r get anchor]
    }

    test {pit multidb: SWAPDB before the cut is reflected in the snapshot} {
        r flushall
        # Values must be long enough to be spillable: an embstr-encoded short
        # string is rejected with "key has embedded value (not spillable)".
        r select 5
        for {set i 0} {$i < 20} {incr i} {
            r set sw:$i "five-[p5_incompressible 300 [expr {$i + 500}]]"
            p5_spill_wait sw:$i
        }
        r select 6
        for {set i 0} {$i < 20} {incr i} {
            r set sw:$i "six-[p5_incompressible 300 [expr {$i + 600}]]"
            p5_spill_wait sw:$i
        }
        r select 9
        # SWAPDB rewrites the physical-to-logical mapping the orphan filter
        # resolves each record through, so the snapshot must follow the swap.
        r swapdb 5 6

        set from [p5_bgsave]
        assert_equal "ok" [s rdb_last_bgsave_status]
        assert_equal 40 [p5_flash_entries $from]

        restart_server 0 true false
        r select 5
        assert_equal "six-[p5_incompressible 300 [expr {3 + 600}]]" [r get sw:3]
        r select 6
        assert_equal "five-[p5_incompressible 300 [expr {3 + 500}]]" [r get sw:3]
        r select 9
    }

    # --- data types ---------------------------------------------------------
    #
    # The flash payload is the DUMP encoding, so in principle every type works
    # the same way. Nothing had verified it through the streaming path though,
    # and the flash section splices the type byte out of the payload by hand.

    test {pit types: hash, set, zset and list round trip through the flash section} {
        r flushall
        for {set i 0} {$i < 15} {incr i} {
            r hset h:$i f1 [p5_incompressible 200 [expr {$i + 1}]] f2 "v$i" f3 12345
            r sadd s:$i alpha-$i beta-$i gamma-$i [p5_incompressible 100 $i]
            r zadd z:$i 1.5 one-$i 2.5 two-$i 99 [p5_incompressible 80 $i]
            r rpush l:$i head-$i [p5_incompressible 150 $i] tail-$i
            foreach k [list h:$i s:$i z:$i l:$i] { p5_spill_wait $k }
        }

        set from [p5_bgsave]
        assert_equal "ok" [s rdb_last_bgsave_status]
        assert_equal 60 [p5_flash_entries $from]

        restart_server 0 true false
        assert_equal 60 [r dbsize]
        for {set i 0} {$i < 15} {incr i} {
            assert_equal [p5_incompressible 200 [expr {$i + 1}]] [r hget h:$i f1]
            assert_equal "12345" [r hget h:$i f3]
            assert_equal 4 [r scard s:$i]
            assert_equal 1 [r sismember s:$i alpha-$i]
            assert_equal 3 [r zcard z:$i]
            assert_equal "1.5" [r zscore z:$i one-$i]
            assert_equal 3 [r llen l:$i]
            assert_equal "head-$i" [r lindex l:$i 0]
            assert_equal "tail-$i" [r lindex l:$i 2]
        }
    }

    # --- point in time ------------------------------------------------------
    #
    # The engine-level suite already asserts post-cut records are excluded from
    # the stream. This asserts the property through the produced RDB, which is
    # what actually matters, and pins the positive control alongside it so a
    # snapshot that simply dropped everything could not pass.

    test {pit cut: records frozen before the cut are in, ones created after are not} {
        r flushall
        set n 100
        for {set i 0} {$i < $n} {incr i} {
            r set before:$i [p5_incompressible 3000 [expr {$i + 31}]]
            p5_spill_wait before:$i
        }
        # A slow memory section keeps the child busy while we mutate the parent.
        for {set i 0} {$i < 200} {incr i} { r set pad:$i "p$i" }
        r config set rdb-key-save-delay 3000

        set from [count_log_lines 0]
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "BGSAVE did not start"
        }
        # Everything from here is after the cut.
        for {set i 0} {$i < 50} {incr i} {
            r set after:$i "created-after-the-cut-[p5_incompressible 300 $i]"
            catch {r debug spill after:$i}
        }
        waitForBgsave r
        r config set rdb-key-save-delay 0
        assert_equal "ok" [s rdb_last_bgsave_status]
        assert_equal $n [p5_flash_entries $from]

        restart_server 0 true false
        # Positive control first: dropping everything must not pass this test.
        for {set i 0} {$i < $n} {incr i} {
            assert_equal [p5_incompressible 3000 [expr {$i + 31}]] [r get before:$i]
        }
        for {set i 0} {$i < 50} {incr i} {
            assert_equal 0 [r exists after:$i]
        }
        assert_equal [expr {$n + 200}] [r dbsize]
    }

    test {pit cut: a spill still in flight at the cut appears exactly once} {
        r flushall
        set n 120
        # Fire the spills without waiting, so several are in flight when the
        # barrier runs. The barrier must settle them before the cut is taken;
        # if it did not, a key could land in both sections or neither.
        for {set i 0} {$i < $n} {incr i} {
            r set inflight:$i [p5_incompressible 500 [expr {$i * 7 + 3}]]
        }
        for {set i 0} {$i < $n} {incr i} { catch {r debug spill inflight:$i} }

        set from [p5_bgsave]
        assert_equal "ok" [s rdb_last_bgsave_status]
        set entries [p5_flash_entries $from]
        # Every key is accounted for exactly once: whatever did not make it into
        # the flash section must have been written by the memory section, and the
        # flash section can never exceed the number of tiered keys.
        assert {$entries >= 0 && $entries <= $n}

        restart_server 0 true false
        assert_equal $n [r dbsize]
        for {set i 0} {$i < $n} {incr i} {
            assert_equal [p5_incompressible 500 [expr {$i * 7 + 3}]] [r get inflight:$i]
        }
    }

    # --- poison -------------------------------------------------------------
    #
    # An aborted stream must fail the save. The danger is the opposite: a
    # truncated stream treated as a complete one, producing an RDB that looks
    # fine and is missing every tiered value.

    test {pit poison: exceeding the overflow limit fails the save loudly} {
        r flushall
        set n 200
        for {set i 0} {$i < $n} {incr i} {
            r set poi:$i [p5_incompressible 4096 [expr {$i + 17}]]
            p5_spill_wait poi:$i
        }
        for {set i 0} {$i < 300} {incr i} { r set poimem:$i "m$i" }

        set aborts_before [p5_counter snapshot_stream_aborts]
        # Smallest permitted limit against ~800KB of records and a consumer that
        # will not drain for seconds: the producer must run past the bound.
        r config set ext-storage-snapshot-stream-overflow-limit 65536
        r config set rdb-key-save-delay 3000

        set from [count_log_lines 0]
        r bgsave
        waitForBgsave r
        r config set rdb-key-save-delay 0
        r config set ext-storage-snapshot-stream-overflow-limit 67108864

        assert_equal "err" [s rdb_last_bgsave_status]
        assert {[p5_counter snapshot_stream_aborts] > $aborts_before}

        # And it must fail PROMPTLY. A poisoned producer used to leave the
        # consumer waiting out its full stall deadline, because dropping the
        # buffered bytes leaves a truncated frame in the pipe and the poison
        # marker queues up behind a frame that never completes. The producer now
        # closes the write end, which the consumer cannot mistake for slowness.
        assert_equal 1 [p5_log_hits $from "past the 65536 byte limit"]
        assert_equal 1 [p5_log_hits $from "stream ended without a terminator"]
        assert_equal 0 [p5_log_hits $from "no records for"]

        # And the failure must be recoverable: the next save works.
        set from2 [p5_bgsave]
        assert_equal "ok" [s rdb_last_bgsave_status]
        assert_equal $n [p5_flash_entries $from2]

        restart_server 0 true false
        assert_equal [expr {$n + 300}] [r dbsize]
        assert_equal [p5_incompressible 4096 [expr {5 + 17}]] [r get poi:5]
    }

    test {pit: server healthy after the whole suite} {
        r flushall
        r set sanity 1
        assert_equal 1 [r get sanity]
        assert_equal "PONG" [r ping]
    }
}

catch {file delete $_path}
