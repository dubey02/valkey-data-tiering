#
# Streaming snapshot under concurrent load (Phase 5)
#
# Everything else in the stream suites snapshots a quiescent-to-lightly-loaded
# store. This runs a snapshot against the conditions the feature will actually
# meet in production, all at once:
#
#   - flash at capacity, so FlashCache must reclaim space (its GC) DURING the save
#   - memory at maxmemory with allkeys-lru, so the engine is spilling and evicting
#   - short-fuse TTLs firing mid-save
#   - sustained write traffic for the whole duration of the save
#
# Why this is the load-bearing test. The streaming path deliberately does NOT
# call extStorageSnapshotPrepare(), which is the only thing that pauses on-flash
# GC and parks the storage IO thread. That is the main win over the fork read
# path, which must pause GC for the child's entire lifetime because it preads
# flash by offset. The design rests on FlashCache's frozen range staying valid
# while GC relocates underneath it -- and nothing had ever exercised that.
#
# What it can and cannot assert. Under eviction and expiry, keys legitimately
# disappear, so "every key survives" is the wrong property. The real properties
# are INTEGRITY (every value that comes back is exactly what was written -- a
# relocated or torn record would show up here), SUBSET (nothing comes back that
# was never written), and POINT IN TIME (nothing written after the cut appears).
#
# The witnesses are asserted, not hoped for: a stress test that cannot prove the
# stress occurred is worse than no test, because it reports green either way.
#
#   EXT_STORAGE_BACKEND=flashcache ./runtest --single unit/data-tiering/ext-storage-snapshot-stream-stress

proc p6_counter {name} {
    set info [r info all]
    if {[regexp "${name}:(\\d+)" $info _ val]} { return $val }
    return 0
}

# One incompressible block, built once. LZF squeezes repeated characters to
# nothing, which would leave the flash file far below capacity and quietly
# disable the GC pressure this whole test depends on.
proc p6_make_base {len} {
    set out ""
    set x 987654321
    while {[string length $out] < $len} {
        set x [expr {($x * 1103515245 + 12345) & 0x7fffffff}]
        append out [format %08x $x]
    }
    return [string range $out 0 [expr {$len - 1}]]
}

# Deterministic and cheap: unique prefix over a shared incompressible body.
# Compression is per value, so sharing the body costs nothing in realism.
proc p6_val {i} { return "$i:$::p6_base" }
proc p6_churn_val {i} { return "$i:$::p6_churn_base" }

proc p6_flash_entries {from} {
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

# FlashCache prints "Head offset : H, Tail offset : T" when a save starts.
#
# Reported for context only, NOT asserted. H < T says the head currently sits
# behind the tail in the log's address space, which is a positional accident of
# where the circular log happens to be when the save begins -- a log that has
# wrapped many times still reports H > T for most of each lap. Asserting it
# produced a false failure on a run whose eviction delta proved reclaim was very
# much active. The eviction delta is the witness that actually means something.
proc p6_log_wrapped {from} {
    set fd [open [srv 0 stdout] r]
    set lines [split [read $fd] "\n"]
    close $fd
    set wrapped 0
    set i 0
    foreach line $lines {
        incr i
        if {$i <= $from} continue
        if {[regexp {Head offset : (\d+), Tail offset : (\d+)} $line _ h t]} {
            if {$h < $t} { set wrapped 1 }
        }
    }
    return $wrapped
}

# Bounded replacement for waitForBgsave, which is `while 1 { ... after 50 }`
# with no timeout: any save that does not complete hangs the suite until the
# harness kills it -- and the harness then deletes the server directory, so the
# log that would explain why is destroyed. Two of six stress runs died that way
# and neither could be classified afterwards.
#
# On breach this captures the state that distinguishes a stalled save from a
# merely slow one, copies the server log somewhere the cleanup will not reach,
# and fails. A failure that explains itself beats a hang every time.
proc p6_wait_bgsave {label {timeout_ms 120000}} {
    set deadline [expr {[clock milliseconds] + $timeout_ms}]
    while {[s rdb_bgsave_in_progress] == 1} {
        if {[clock milliseconds] > $deadline} {
            set stamp [clock seconds]
            set dest "/tmp/p11-$label-$stamp.log"
            catch {file copy -force [srv 0 stdout] $dest}

            # Every field is read through catch: a typo or a field that only
            # appears while a save is live must not throw and mask the real
            # failure, which is the whole point of this diagnostic.
            proc _p6f {name} {
                if {[catch {s $name} v]} { return "n/a" }
                return $v
            }
            set diag "save did not complete within ${timeout_ms}ms at '$label'"
            append diag "\n  in_progress=[_p6f rdb_bgsave_in_progress]"
            append diag " last_status=[_p6f rdb_last_bgsave_status]"
            append diag " bgsave_time_sec=[_p6f rdb_current_bgsave_time_sec]"
            append diag " keys_done=[_p6f current_save_keys_processed]"
            append diag "/[_p6f current_save_keys_total]"
            append diag " fork_perc=[_p6f current_fork_perc]"
            append diag "\n  on_flash=[p6_counter num_items_on_flash]"
            append diag " fc_evicted=[p6_counter fc_num_items_evicted]"
            append diag " fc_active_bytes=[p6_counter fc_active_memory_bytes]"
            append diag "\n  stream armed=[p6_counter snapshot_stream_armed]"
            append diag " last_records=[p6_counter snapshot_stream_last_records]"
            append diag " backpressure=[p6_counter snapshot_stream_backpressure_events]"
            append diag " overflow_peak=[p6_counter snapshot_stream_overflow_peak_bytes]"
            append diag " aborts=[p6_counter snapshot_stream_aborts]"
            append diag "\n  server log preserved at $dest"
            fail $diag
        }
        after 100
    }
}

proc p6_log_hits {from pattern} {
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
# This suite is specific to real FlashCache: it is about FlashCache's own log
# reclaim, and the mock backend has no equivalent -- its eviction counter would
# never move, so the witnesses below could not be asserted. Skip rather than
# fail, so a default (mock) run does not report a red that means nothing.
if {$_backend ne "flashcache"} {
    puts "Skipping ext-storage-snapshot-stream-stress: requires EXT_STORAGE_BACKEND=flashcache (got '$_backend')"
    return
}

set _path "/tmp/fc-test-snapstress-[pid].db"
# Deliberately small: the flash file has to FILL so FlashCache reclaims during
# the save. Must match ext-storage-capacity-mb or FC asserts on the file size.
catch {exec fallocate -l 64M $_path}

set ::p6_base [p6_make_base 16384]
set ::p6_churn_base [p6_make_base 1024]
set ::p6_seed 5000

start_server [list tags {"ext-storage" "ext-storage-snapshot-stream-stress"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path \
    ext-storage-capacity-mb 64 \
    maxmemory 32mb \
    maxmemory-policy allkeys-lru \
    appendonly no \
    save {} \
    hz 100 \
]] {

    test {stress: fill flash to capacity and confirm the pressure is real} {
        r flushall
        # ~96MB of incompressible payload against a 64MB flash file, which is
        # the configured minimum size. Live data therefore exceeds the log and
        # FlashCache has to evict records, not merely relocate them.
        # OOM replies are expected backpressure here, not a failure.
        for {set i 0} {$i < $::p6_seed} {incr i} {
            catch {r set st:$i [p6_val $i]}
        }
        wait_for_condition 100 200 {
            [p6_counter total_num_items_spilled_to_ext_storage] > 0
        } else {
            fail "engine never spilled to flash"
        }
        assert {[p6_counter num_items_on_flash] > 0}
        # Enough of the live set has to be on flash for the log to be under real
        # pressure. Reclaim itself lags the writes, so it is asserted in the next
        # test against the save window rather than here -- checking it at the end
        # of seeding just races the reclaim thread.
        assert {[p6_counter num_items_on_flash] > 1000}
        assert {[p6_counter fc_active_memory_bytes] > 0}
    }

    test {stress: snapshot with GC, eviction, expiry and writes all live} {
        # Short-fuse TTLs, written last so they are MRU and not LRU-evicted
        # before they get the chance to expire. Deliberately generous in count
        # and short in fuse: most of these are reclaimed by flash eviction before
        # their TTL fires, so a thin batch leaves the expiry witness at risk of
        # reading zero on a differently-timed run and reporting a false failure.
        # Small payloads too, so seeding them does not itself take seconds.
        set ttl_deadline [expr {[clock milliseconds] + 30000}]
        set ttl_written 0
        for {set i 0} {$i < 1500} {incr i} {
            catch {r set ttl:$i [p6_churn_val $i] px [expr {300 + ($i % 500)}]}
            incr ttl_written
            if {[clock milliseconds] > $ttl_deadline} break
        }
        assert {$ttl_written > 500}

        set evicted_before [p6_counter fc_num_items_evicted]
        set expired_before [p6_counter expired_keys]
        set spilled_before [p6_counter total_num_items_spilled_to_ext_storage]

        # Stretch the save so the churn below overlaps all of it.
        r config set rdb-key-save-delay 400
        set from [count_log_lines 0]
        r bgsave
        wait_for_condition 100 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "BGSAVE did not start"
        }

        # Sustained traffic for the whole save. Every one of these is after the
        # cut, so none of them may appear in the snapshot -- which is the point
        # in time property under load rather than at rest.
        set churn 0
        set churn_deadline [expr {[clock milliseconds] + 60000}]
        while {[s rdb_bgsave_in_progress] == 1} {
            for {set j 0} {$j < 40} {incr j} {
                catch {r set churn:$churn [p6_churn_val $churn]}
                incr churn
            }
            if {$churn > 20000} break
            if {[clock milliseconds] > $churn_deadline} {
                fail "write traffic stalled for 60s while a save was in flight (churn=$churn)"
            }
        }
        p6_wait_bgsave "first-save"
        r config set rdb-key-save-delay 0

        assert_equal "ok" [s rdb_last_bgsave_status]
        assert {$churn > 100}

        # The witnesses. Without these the test reports green even if the save
        # happened over a quiet store, which is exactly the failure mode of a
        # stress test nobody checked.
        set evicted_delta [expr {[p6_counter fc_num_items_evicted] - $evicted_before}]
        set expired_delta [expr {[p6_counter expired_keys] - $expired_before}]
        set spilled_delta [expr {[p6_counter total_num_items_spilled_to_ext_storage] - $spilled_before}]
        puts "    load during save: fc_evicted=+$evicted_delta expired=+$expired_delta spilled=+$spilled_delta churn_writes=$churn flash_entries=[p6_flash_entries $from]"
        assert {$evicted_delta > 0}
        assert {$expired_delta > 0}
        assert {$spilled_delta > 0}
        # Positional context only -- see p6_log_wrapped. Reclaim is proven by
        # evicted_delta above: FlashCache only drops records when it has to make
        # room, so a non-zero delta across the save window means the reclaimer
        # was running while the cut was taken.
        puts "    log head-behind-tail at cut: [p6_log_wrapped $from] (informational)"

        # The stream must have carried records and must not have aborted.
        assert {[p6_flash_entries $from] > 0}
        assert_equal 0 [p6_log_hits $from "aborting stream"]
        assert_equal 0 [p6_log_hits $from "no records for"]
        assert_equal 0 [p6_log_hits $from "falling back to the fork read path"]
    }

    test {stress: every value that survives the reload is byte for byte correct} {
        restart_server 0 true false

        # INTEGRITY. A record relocated by GC underneath the frozen range, or a
        # torn frame, shows up here as a wrong or truncated value. Eviction and
        # expiry are free to have removed keys; they are not free to corrupt the
        # ones that remain.
        set present 0
        set checked 0
        for {set i 0} {$i < $::p6_seed} {incr i} {
            set v [r get st:$i]
            if {$v eq ""} continue
            incr present
            # Compare cheaply first, then only build the expected string on a
            # mismatch -- 6000 full comparisons of 8KB is needlessly slow.
            if {$v ne [p6_val $i]} {
                fail "st:$i came back corrupted (len [string length $v], expected [string length [p6_val $i]])"
            }
            incr checked
        }
        assert {$present > 0}
        assert_equal $present $checked

        # SUBSET / POINT IN TIME: nothing written after the cut may be in the
        # snapshot, however much traffic was in flight when it was taken.
        set leaked 0
        for {set i 0} {$i < 200} {incr i} {
            if {[r exists churn:$i]} { incr leaked }
        }
        assert_equal 0 $leaked
    }

    test {stress: a second snapshot under the same load still succeeds} {
        # Latched transport state would show up on the save after a loaded one.
        set evicted_before [p6_counter fc_num_items_evicted]
        r config set rdb-key-save-delay 300
        set from [count_log_lines 0]
        r bgsave
        wait_for_condition 100 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "second BGSAVE did not start"
        }
        set churn2 0
        set churn2_deadline [expr {[clock milliseconds] + 60000}]
        while {[s rdb_bgsave_in_progress] == 1} {
            for {set j 0} {$j < 40} {incr j} {
                catch {r set churn2:$churn2 [p6_churn_val $churn2]}
                incr churn2
            }
            if {$churn2 > 20000} break
            if {[clock milliseconds] > $churn2_deadline} {
                fail "write traffic stalled for 60s during the second save (churn=$churn2)"
            }
        }
        p6_wait_bgsave "second-save"
        r config set rdb-key-save-delay 0

        assert_equal "ok" [s rdb_last_bgsave_status]
        assert {[p6_counter fc_num_items_evicted] > $evicted_before}
        assert {[p6_flash_entries $from] > 0}
        assert_equal 0 [p6_log_hits $from "aborting stream"]
    }

    test {stress: server healthy and still serving after the storm} {
        assert_equal "PONG" [r ping]
        assert {[r dbsize] > 0}
        r set post:storm "still-working"
        assert_equal "still-working" [r get post:storm]
    }
}

catch {file delete $_path}
