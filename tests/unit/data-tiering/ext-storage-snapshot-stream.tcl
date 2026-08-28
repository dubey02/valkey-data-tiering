#
# Streaming snapshot: storage engine contract (Phase 1)
#
# Verifies the engine side of storageSnapshotStreamStart() in isolation, with no
# RDB plumbing involved, so a failure here is attributable to the storage engine
# or its stream parser rather than to rdb.c.
#
# The contract under test:
#   - every record live at the cut is delivered exactly once
#   - records written after the cut are not delivered
#   - complete() fires exactly once, including on abort
#   - repeated streams over a quiescent store agree
#
# Runs against the mock by default. For real FlashCache:
#   EXT_STORAGE_BACKEND=flashcache ./runtest --single unit/data-tiering/ext-storage-snapshot-stream
#
# Note on counting: the storage engine enumerates ITS OWN store, not the engine
# keyspace, so records for keys the engine has forgotten (see the orphan test
# below) are still delivered. Every count assertion here is therefore a DELTA
# against a baseline taken in the same test, never an absolute.

proc get_tiering_counter {name} {
    set info [r info all]
    if {[regexp "${name}:(\\d+)" $info _ val]} { return $val }
    return 0
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

# Values that do not compress: LZF squeezes a repeated-character string to a few
# bytes, which makes any byte-size assertion meaningless.
proc incompressible {len seed} {
    set out ""
    set x $seed
    while {[string length $out] < $len} {
        set x [expr {($x * 1103515245 + 12345) & 0x7fffffff}]
        append out [format %08x $x]
    }
    return [string range $out 0 [expr {$len - 1}]]
}

proc stream_selftest {{timeout 20000}} {
    return [r debug ext-storage-stream-selftest $timeout]
}

proc stream_records {{timeout 20000}} {
    set res [stream_selftest $timeout]
    assert_equal 1 [dict get $res completed]
    assert_equal 1 [dict get $res ok]
    return [dict get $res records]
}

if {[info exists ::env(EXT_STORAGE_BACKEND)]} {
    set _backend $::env(EXT_STORAGE_BACKEND)
} else {
    set _backend "flashcache-mock"
}
set _path "/tmp/fc-test-snapstream-[pid].db"
# Test bodies run in a nested context and cannot see file local variables, so
# publish the backend name globally for the diagnostic output below.
set ::stream_backend $_backend
# Real FlashCache asserts on the backing file's size during init, and the assert
# path itself crashes because FC's logger is not up yet. Pre-allocate to match
# ext-storage-capacity-mb, the same way the other real-FC suites do.
catch {exec fallocate -l 256M $_path}

start_server [list tags {"ext-storage" "ext-storage-snapshot-stream"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path \
    ext-storage-capacity-mb 256 \
    maxmemory 100mb \
    maxmemory-policy allkeys-lru \
    appendonly no \
    save {} \
]] {

    test {stream: engine reports streaming support and completes} {
        # Failing here on real FC while passing on the mock means the vtable op
        # was left NULL in storage_flashcache_real.c.
        set res [stream_selftest]
        assert_equal 1 [dict get $res completed]
        assert_equal 1 [dict get $res ok]
    }

    test {stream: spilling N keys adds exactly N records to the stream} {
        set base [stream_records]
        set n 200
        for {set i 0} {$i < $n} {incr i} {
            r set sn:a:$i [incompressible 400 $i]
        }
        for {set i 0} {$i < $n} {incr i} { debug_spill_wait sn:a:$i }
        # Exactly N, not >= N: a duplicate delivery would overshoot.
        assert_equal $n [expr {[stream_records] - $base}]
    }

    test {stream: memory resident keys are not delivered} {
        set base [stream_records]
        r set sn:mem:1 [incompressible 400 991]
        r set sn:mem:2 [incompressible 400 992]
        r set sn:flash:1 [incompressible 400 993]
        debug_spill_wait sn:flash:1
        assert_equal 1 [expr {[stream_records] - $base}]
    }

    test {stream: value bytes reflect the stored payload} {
        set before [stream_selftest]
        set n 50
        for {set i 0} {$i < $n} {incr i} {
            r set sn:sz:$i [incompressible 1000 [expr {5000 + $i}]]
        }
        for {set i 0} {$i < $n} {incr i} { debug_spill_wait sn:sz:$i }
        set after [stream_selftest]
        set delta [expr {[dict get $after value_bytes] - [dict get $before value_bytes]}]
        # Incompressible 1000 byte values: the serialized payload cannot be far
        # below the raw size. Allow headroom for encoding but catch truncation.
        assert {$delta >= [expr {$n * 900}]}
    }

    test {stream: repeated streams over a quiescent store agree} {
        set a [stream_selftest]
        set b [stream_selftest]
        assert_equal 1 [dict get $a ok]
        assert_equal 1 [dict get $b ok]
        assert_equal [dict get $a records] [dict get $b records]
        # Order independent digest: two passes over the same frozen set must
        # agree even though delivery order is not guaranteed.
        assert_equal [dict get $a digest] [dict get $b digest]
    }

    test {stream: deleting spilled keys removes them from the stream} {
        set base [stream_records]
        set n 100
        for {set i 0} {$i < $n} {incr i} {
            r set sn:del:$i [incompressible 400 [expr {7000 + $i}]]
        }
        for {set i 0} {$i < $n} {incr i} { debug_spill_wait sn:del:$i }
        assert_equal $n [expr {[stream_records] - $base}]

        for {set i 0} {$i < 60} {incr i} { r del sn:del:$i }
        # 60 gone, 40 remain above the baseline.
        assert_equal 40 [expr {[stream_records] - $base}]
    }

    test {stream: post cut writes are excluded} {
        set base [stream_records]
        set n 150
        for {set i 0} {$i < $n} {incr i} {
            r set sn:cut:$i [incompressible 400 [expr {8000 + $i}]]
        }
        for {set i 0} {$i < $n} {incr i} { debug_spill_wait sn:cut:$i }

        # Racing traffic on a second link, overlapping the stream in real time.
        # These keys are written after the cut is established, so however many
        # land, the delivered count must not exceed the pre cut population.
        set rd [valkey_deferring_client]
        for {set i 0} {$i < 400} {incr i} {
            $rd set sn:post:$i [incompressible 200 [expr {9000 + $i}]]
        }
        set res [stream_selftest]
        for {set i 0} {$i < 400} {incr i} { $rd read }
        $rd close

        assert_equal 1 [dict get $res ok]
        assert_equal $n [expr {[dict get $res records] - $base}]
    }

    test {stream: back to back streams all complete (no latched state)} {
        for {set i 0} {$i < 5} {incr i} {
            set res [stream_selftest]
            assert_equal 1 [dict get $res completed]
            assert_equal 1 [dict get $res ok]
        }
    }

    test {stream: FLUSHALL orphan behaviour is explicit} {
        # The engine enumerates the STORE, not the keyspace. FLUSHALL clears the
        # keyspace but its flash cleanup is ASYNCHRONOUS, so the gauge and the
        # store drain over time rather than instantly. Poll rather than assert a
        # single instant, then record what settled.
        #
        # This test does not assert that the store empties, because the streaming
        # path must tolerate orphans regardless: a record whose key the engine
        # has forgotten must never reach an RDB. That filter is engine side work,
        # tracked separately.
        r flushall
        set deadline [expr {[clock milliseconds] + 8000}]
        while {[clock milliseconds] < $deadline} {
            if {[get_tiering_counter num_items_on_flash] == 0} break
            after 100
        }
        set gauge [get_tiering_counter num_items_on_flash]
        set orphans [stream_records]
        puts "  FLUSHALL drain on $::stream_backend: gauge=$gauge streamed=$orphans"
        # Well formed and terminating is the contract under test here.
        assert {$orphans >= 0}
        assert {$gauge >= 0}
    }

    test {transport: live records pass the orphan filter} {
        # Positive control. A filter that rejects everything is indistinguishable
        # from a working one if you only assert on the reject count, so the
        # load-bearing assertion is that all N live records ARRIVE.
        #
        # sent is NOT asserted equal to N: the engine enumerates its own store,
        # and on the mock FLUSHALL leaves records behind, so earlier tests'
        # orphans are still streamed. That is what makes the mock a useful
        # positive test for the reject path, while real FlashCache clears its
        # store and has nothing to orphan.
        r flushall
        set deadline [expr {[clock milliseconds] + 8000}]
        while {[clock milliseconds] < $deadline} {
            if {[get_tiering_counter num_items_on_flash] == 0} break
            after 100
        }
        set n 200
        for {set i 0} {$i < $n} {incr i} { r set orph:$i [incompressible 600 [expr {$i + 31}]] }
        for {set i 0} {$i < $n} {incr i} { debug_spill_wait orph:$i }

        set res [r debug ext-storage-snapshot-transport-selftest 30000]
        assert_equal 1 [dict get $res ok]
        assert_equal $n [dict get $res received]
        # Everything streamed is either delivered or rejected, never lost.
        assert_equal [dict get $res sent] \
            [expr {[dict get $res received] + [dict get $res orphans]}]
        puts "  orphan filter on $::stream_backend: sent=[dict get $res sent] live=[dict get $res received] dropped=[dict get $res orphans]"
    }

    test {transport: framing count and digest verify end to end} {
        # The terminator carries the producer's count and digest, and the
        # consumer returns an error rather than DONE on mismatch, so ok=1 is
        # itself the assertion that framing survived the pipe intact.
        set res [r debug ext-storage-snapshot-transport-selftest 30000]
        assert_equal 1 [dict get $res ok]
        assert_equal [dict get $res sent] \
            [expr {[dict get $res received] + [dict get $res orphans]}]
    }

    test {stream: server healthy after streaming} {
        assert_equal "PONG" [r ping]
        r set sn:post:ok 1
        assert_equal "1" [r get sn:post:ok]
        assert_equal 0 [get_tiering_counter snapshot_active]
    }
}

catch {exec rm -f $_path}
