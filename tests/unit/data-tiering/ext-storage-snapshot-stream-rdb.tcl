#
# Streaming snapshot: RDB integration (Phase 3)
#
# Phase 1 verified the storage engine's stream contract and Phase 2 the
# transport. This suite verifies the part that actually produces a snapshot: the
# memory section skips tiered keys, the flash section emits them from the
# pre-fork stream, and the resulting file is a plain RDB that loads back
# identically.
#
# Every test that claims the streaming path ran asserts on the flash section log
# line. Without that assertion a silent fall back to the fork read path would
# still produce a correct RDB and the test would pass vacuously.
#
# Runs against the mock by default. For real FlashCache:
#   EXT_STORAGE_BACKEND=flashcache ./runtest --single unit/data-tiering/ext-storage-snapshot-stream-rdb

proc rdb_get_counter {name} {
    set info [r info all]
    if {[regexp "${name}:(\\d+)" $info _ val]} { return $val }
    return 0
}

proc rdb_spill_wait {key {timeout 5000}} {
    set before [rdb_get_counter total_num_items_spilled_to_ext_storage]
    r debug spill $key
    set start [clock milliseconds]
    while {1} {
        if {[rdb_get_counter total_num_items_spilled_to_ext_storage] > $before} { return }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill of '$key'"
        }
        after 50
    }
}

proc rdb_incompressible {len seed} {
    set out ""
    set x $seed
    while {[string length $out] < $len} {
        set x [expr {($x * 1103515245 + 12345) & 0x7fffffff}]
        append out [format %08x $x]
    }
    return [string range $out 0 [expr {$len - 1}]]
}

# Run a BGSAVE and return the number of entries the flash section wrote, or -1
# when the streaming path did not run at all. Reading it from the log rather
# than inferring it from key counts is what distinguishes "streamed correctly"
# from "fell back and happened to be correct".
# Entries the flash section reported after line $from, or -1 when it did not run.
proc flash_entries_since {from} {
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

proc bgsave_flash_entries {} {
    set from [count_log_lines 0]
    r bgsave
    waitForBgsave r
    assert_equal "ok" [s rdb_last_bgsave_status]
    return [flash_entries_since $from]
}

# Occurrences of $pattern in the server log after line $from. Needed because
# count_message_lines scans the whole file, and earlier tests in this suite have
# already written the lines we are asking about.
proc log_hits_since {from pattern} {
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

# Last "Background saving started by pid N" in the log after line $from.
proc bgsave_child_pid {from} {
    set fd [open [srv 0 stdout] r]
    set lines [split [read $fd] "\n"]
    close $fd
    set pid 0
    set i 0
    foreach line $lines {
        incr i
        if {$i <= $from} continue
        if {[regexp {Background saving started by pid (\d+)} $line _ p]} { set pid $p }
    }
    return $pid
}

if {[info exists ::env(EXT_STORAGE_BACKEND)]} {
    set _backend $::env(EXT_STORAGE_BACKEND)
} else {
    set _backend "flashcache-mock"
}
set _path "/tmp/fc-test-snaprdb-[pid].db"
catch {exec fallocate -l 256M $_path}

start_server [list tags {"ext-storage" "ext-storage-snapshot-stream-rdb"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path \
    ext-storage-capacity-mb 256 \
    maxmemory 100mb \
    maxmemory-policy allkeys-lru \
    appendonly no \
    save {} \
]] {

    test {rdb stream: flash section carries every tiered key} {
        r flushall
        set n 200
        for {set i 0} {$i < $n} {incr i} {
            r set flash:$i [rdb_incompressible 512 [expr {$i + 7}]]
            rdb_spill_wait flash:$i
        }
        # Memory-resident keys go through the memory section and must not appear
        # in the flash section's tally.
        for {set i 0} {$i < 50} {incr i} { r set mem:$i "v$i" }

        set entries [bgsave_flash_entries]
        assert_equal $n $entries
    }

    test {rdb stream: reload restores tiered values byte for byte} {
        r flushall
        set n 150
        array set expected {}
        for {set i 0} {$i < $n} {incr i} {
            set v [rdb_incompressible [expr {256 + $i}] [expr {$i * 31 + 5}]]
            set expected($i) $v
            r set flash:$i $v
            rdb_spill_wait flash:$i
        }
        for {set i 0} {$i < 40} {incr i} { r set mem:$i "mem-value-$i" }

        assert_equal $n [bgsave_flash_entries]
        restart_server 0 true false

        assert_equal [expr {$n + 40}] [r dbsize]
        for {set i 0} {$i < $n} {incr i} {
            assert_equal $expected($i) [r get flash:$i]
        }
        for {set i 0} {$i < 40} {incr i} {
            assert_equal "mem-value-$i" [r get mem:$i]
        }
    }

    test {rdb stream: TTLs on tiered keys survive the round trip} {
        r flushall
        for {set i 0} {$i < 30} {incr i} {
            r set ttl:$i [rdb_incompressible 300 [expr {$i + 101}]]
            r expire ttl:$i 3000
            rdb_spill_wait ttl:$i
        }
        for {set i 0} {$i < 30} {incr i} {
            r set nottl:$i [rdb_incompressible 300 [expr {$i + 202}]]
            rdb_spill_wait nottl:$i
        }

        assert_equal 60 [bgsave_flash_entries]
        restart_server 0 true false

        for {set i 0} {$i < 30} {incr i} {
            set t [r ttl ttl:$i]
            assert {$t > 2000 && $t <= 3000}
            assert_equal -1 [r ttl nottl:$i]
        }
    }

    test {rdb stream: deleted keys are not resurrected} {
        r flushall
        for {set i 0} {$i < 100} {incr i} {
            r set gone:$i [rdb_incompressible 400 [expr {$i + 11}]]
            rdb_spill_wait gone:$i
        }
        # Delete half. Their records may still sit inside the frozen range until
        # the space is reclaimed, so the orphan filter is the only thing keeping
        # them out of the RDB.
        for {set i 0} {$i < 100} {incr i 2} { r del gone:$i }
        assert_equal 50 [r dbsize]

        set entries [bgsave_flash_entries]
        assert_equal 50 $entries
        restart_server 0 true false

        assert_equal 50 [r dbsize]
        for {set i 0} {$i < 100} {incr i} {
            if {$i % 2 == 0} {
                assert_equal 0 [r exists gone:$i]
            } else {
                assert_equal 1 [r exists gone:$i]
            }
        }
    }

    test {rdb stream: keys fetched back to memory are written once} {
        r flushall
        for {set i 0} {$i < 60} {incr i} {
            r set both:$i [rdb_incompressible 320 [expr {$i + 71}]]
            rdb_spill_wait both:$i
        }
        # Reading pulls these back into memory. Their flash records linger, so
        # the same key is reachable from both sections; the filter must reject
        # the flash copy so the key is not written twice.
        for {set i 0} {$i < 20} {incr i} { r get both:$i }

        set entries [bgsave_flash_entries]
        assert {$entries <= 60}
        restart_server 0 true false

        assert_equal 60 [r dbsize]
        for {set i 0} {$i < 60} {incr i} {
            assert_equal [rdb_incompressible 320 [expr {$i + 71}]] [r get both:$i]
        }
    }

    test {rdb stream: a killed child leaves the stream releasable} {
        r flushall
        for {set i 0} {$i < 80} {incr i} {
            r set k:$i [rdb_incompressible 400 [expr {$i + 3}]]
            rdb_spill_wait k:$i
        }

        # Slow the child down enough to kill it mid-save.
        r config set rdb-key-save-delay 1000
        set from [count_log_lines 0]
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "BGSAVE did not start"
        }
        set childpid [bgsave_child_pid $from]
        assert {$childpid > 0}
        catch {exec kill -9 $childpid}
        r config set rdb-key-save-delay 0
        wait_for_condition 100 100 {
            [s rdb_bgsave_in_progress] == 0
        } else {
            fail "BGSAVE did not finish after the child was killed"
        }

        # The interesting assertion is not the failed save, it is that the next
        # one works: a leaked transport would make Arm refuse.
        assert_equal 80 [bgsave_flash_entries]
    }

    test {rdb stream: server healthy afterwards} {
        r flushall
        r set sanity 1
        assert_equal 1 [r get sanity]
        assert_equal "PONG" [r ping]
    }

    test {rdb fork path: still produces an identical snapshot} {
        # Streaming is the default now, which would leave the fork read path
        # untested. Force it off and assert the fallback still round trips, so a
        # regression there is caught here rather than in production.
        r flushall
        r debug ext-storage-snapshot-stream 0
        set n 120
        array set expected {}
        for {set i 0} {$i < $n} {incr i} {
            set v [rdb_incompressible [expr {200 + $i}] [expr {$i * 17 + 3}]]
            set expected($i) $v
            r set fk:$i $v
            r expire fk:$i 3000
            rdb_spill_wait fk:$i
        }

        set from [count_log_lines 0]
        r bgsave
        waitForBgsave r
        assert_equal "ok" [s rdb_last_bgsave_status]
        # No flash section: the fork path materializes inside rdbSaveKeyValuePair.
        assert_equal 0 [log_hits_since $from "flash section"]
        assert {[log_hits_since $from "falling back to the fork read path"] == 0}

        restart_server 0 true false
        assert_equal $n [r dbsize]
        for {set i 0} {$i < $n} {incr i} {
            assert_equal $expected($i) [r get fk:$i]
            set t [r ttl fk:$i]
            assert {$t > 2000 && $t <= 3000}
        }
    }

    test {rdb: streaming resumes after the override is cleared} {
        r debug ext-storage-snapshot-stream 1
        r flushall
        for {set i 0} {$i < 40} {incr i} {
            r set back:$i [rdb_incompressible 260 [expr {$i + 91}]]
            rdb_spill_wait back:$i
        }
        assert_equal 40 [bgsave_flash_entries]
    }

    # --- foreground SAVE ----------------------------------------------------
    #
    # No fork here: the main thread arms the cut and drains it while the storage
    # IO thread produces. Blocking on the pipe would hang the server forever
    # because the write end is open in this same process, so the flash section
    # runs non-blocking against a stall deadline.

    test {rdb SAVE: foreground save streams the flash section} {
        r flushall
        set n 120
        array set expected {}
        for {set i 0} {$i < $n} {incr i} {
            set v [rdb_incompressible [expr {220 + $i}] [expr {$i * 13 + 9}]]
            set expected($i) $v
            r set fg:$i $v
            rdb_spill_wait fg:$i
        }
        for {set i 0} {$i < 25} {incr i} { r set fgmem:$i "m$i" }

        set from [count_log_lines 0]
        r save
        assert_equal $n [flash_entries_since $from]

        restart_server 0 true false
        assert_equal [expr {$n + 25}] [r dbsize]
        for {set i 0} {$i < $n} {incr i} { assert_equal $expected($i) [r get fg:$i] }
        assert_equal "m3" [r get fgmem:3]
    }

    test {rdb SAVE: DEBUG RELOAD round trips tiered values} {
        r flushall
        set n 80
        array set expected {}
        for {set i 0} {$i < $n} {incr i} {
            set v [rdb_incompressible [expr {180 + $i}] [expr {$i * 23 + 4}]]
            set expected($i) $v
            r set dr:$i $v
            r expire dr:$i 3000
            rdb_spill_wait dr:$i
        }

        set from [count_log_lines 0]
        r debug reload
        assert_equal $n [flash_entries_since $from]

        assert_equal $n [r dbsize]
        for {set i 0} {$i < $n} {incr i} {
            assert_equal $expected($i) [r get dr:$i]
            set t [r ttl dr:$i]
            assert {$t > 2000 && $t <= 3000}
        }
    }

    test {rdb SAVE: repeated foreground saves do not latch the transport} {
        r flushall
        for {set i 0} {$i < 30} {incr i} {
            r set rep:$i [rdb_incompressible 200 [expr {$i + 55}]]
            rdb_spill_wait rep:$i
        }
        for {set j 0} {$j < 3} {incr j} {
            set from [count_log_lines 0]
            r save
            assert_equal 30 [flash_entries_since $from]
        }
    }

    test {rdb SAVE: fork path parity for the foreground save} {
        r flushall
        r debug ext-storage-snapshot-stream 0
        for {set i 0} {$i < 60} {incr i} {
            r set fgf:$i [rdb_incompressible 240 [expr {$i + 37}]]
            rdb_spill_wait fgf:$i
        }
        set from [count_log_lines 0]
        r save
        assert_equal 0 [log_hits_since $from "flash section"]
        r debug reload
        assert_equal 60 [r dbsize]
        assert_equal [rdb_incompressible 240 [expr {11 + 37}]] [r get fgf:11]
        r debug ext-storage-snapshot-stream 1
    }

    # --- backpressure ------------------------------------------------------
    #
    # Every test above keeps the whole stream inside the pipe buffer, so the
    # producer never has to buffer and the overflow path never runs. This forces
    # it: enough flash bytes to overflow the pipe, and a memory section slow
    # enough that the consumer is not draining while the producer works.
    #
    # The failure this guards against is silent. A terminator left in overflow is
    # never flushed by the engine -- which has stopped calling back, or aborted
    # outright the first time the sink reported full -- so the consumer waits out
    # its stall deadline and fails a snapshot that was in fact produced in full.

    test {rdb stream: a stream that overflows the pipe still terminates} {
        r flushall
        r debug ext-storage-snapshot-stream 1
        set n 200
        for {set i 0} {$i < $n} {incr i} {
            r set ov:$i [rdb_incompressible 4096 [expr {$i * 41 + 13}]]
            rdb_spill_wait ov:$i
        }
        # Slow memory section: the consumer will not touch the pipe for a while.
        for {set i 0} {$i < 300} {incr i} { r set ovmem:$i "m$i" }
        r config set rdb-key-save-delay 2000

        set from [count_log_lines 0]
        r bgsave
        waitForBgsave r
        r config set rdb-key-save-delay 0
        assert_equal "ok" [s rdb_last_bgsave_status]
        assert_equal $n [flash_entries_since $from]

        restart_server 0 true false
        assert_equal [expr {$n + 300}] [r dbsize]
        for {set i 0} {$i < $n} {incr i} {
            assert_equal [rdb_incompressible 4096 [expr {$i * 41 + 13}]] [r get ov:$i]
        }
    }
}

# Fresh server: nothing has ever been spilled, so no cut is taken and neither
# path is engaged. A separate server rather than a FLUSHALL because
# num_items_on_flash is only moved by storage completions and is not reset on
# flush, so after a FLUSHALL it still reads non-zero.
set _path2 "/tmp/fc-test-snaprdb-empty-[pid].db"
catch {exec fallocate -l 256M $_path2}
start_server [list tags {"ext-storage" "ext-storage-snapshot-stream-rdb"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path2 \
    ext-storage-capacity-mb 256 \
    maxmemory 100mb \
    maxmemory-policy allkeys-lru \
    appendonly no \
    save {} \
]] {

    test {rdb stream: save succeeds with nothing on flash} {
        assert_equal 0 [rdb_get_counter num_items_on_flash]
        for {set i 0} {$i < 20} {incr i} { r set only:mem:$i "v$i" }

        r bgsave
        waitForBgsave r
        assert_equal "ok" [s rdb_last_bgsave_status]
        restart_server 0 true false
        assert_equal 20 [r dbsize]
        assert_equal "v7" [r get only:mem:7]
    }
}

catch {file delete $_path}
catch {file delete $_path2}
