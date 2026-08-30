# Data tiering + bloom module (OBJ_MODULE, Rust) compatibility tests.
#
# Requires libvalkey_bloom.so from valkey-io/valkey-bloom (cargo build
# --release). Set BLOOM_MODULE_PATH to the .so; tests are skipped when unset.
#
#   BLOOM_MODULE_PATH=/tmp/valkey-bloom/target/release/libvalkey_bloom.so \
#     ./runtest --single unit/data-tiering/ext-storage-bloom-module
#
# Complements ext-storage-json-module.tcl: bloom is a Rust module
# (valkey-module-rs bindings) with different rdb_save/rdb_load and allocation
# patterns exercised on the IO threads during spill/fetch. Includes regression
# coverage for the moduleNotifyKeyUnlink tiered-placeholder crash (DEL/expiry
# of a flash-resident module key), fixed by the !objectIsTiered() guard in
# module.c.
#
# Known POC-wide quirk (not bloom-specific, also affects strings): DEL of a
# flash-resident key replies 0 even though the key is deleted — the blocking
# DEL re-executes after the flash-delete completion has already removed the
# key. Tests assert on EXISTS, not on the DEL reply.

if {![info exists ::env(BLOOM_MODULE_PATH)] || ![file exists $::env(BLOOM_MODULE_PATH)]} {
    if {$::verbose} { puts "Skipping bloom-module tiering tests: BLOOM_MODULE_PATH not set" }
    return
}
set ::bloom_module_path $::env(BLOOM_MODULE_PATH)

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

# Real FlashCache asserts on backing-file size inside getFileSize() before its
# logger exists, so a missing or undersized file segfaults during init with no
# usable message (the harness only reports "Can't start / No PID detected").
# Pre-allocating removes that entirely. Harmless for the mock backend.
set _fcpath "/tmp/valkey-flash-bloom-[pid].db"
catch {exec fallocate -l 256M $_fcpath}

start_server [list tags {"ext-storage" "ext-storage-bloom-module"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache-mock \
    ext-storage-path $_fcpath \
    ext-storage-capacity-mb 256 \
    maxmemory 8mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command yes \
    loadmodule $::bloom_module_path \
]] {

    test {BLOOM module: baseline ops with tiering enabled} {
        assert_equal 1 [r bf.add basefilter item1]
        assert_equal 1 [r bf.add basefilter item2]
        assert_equal 1 [r bf.exists basefilter item1]
        assert_equal 0 [r bf.exists basefilter neveradded]
        assert_equal {bloomfltr} [r type basefilter]
    }

    test {BLOOM module: explicit spill of a filter succeeds} {
        r bf.add spillfilter itemA
        r bf.add spillfilter itemB
        debug_spill_wait spillfilter
        assert {[get_tiering_counter num_items_on_flash] >= 1}
        # Server must survive the spill (IO-thread rdb_save of Rust module value)
        assert_equal {PONG} [r ping]
    }

    test {BLOOM module: fetch spilled filter round-trip preserves membership} {
        # spillfilter is on flash; BF.EXISTS forces a fetch + rdb_load
        assert_equal 1 [r bf.exists spillfilter itemA]
        assert_equal 1 [r bf.exists spillfilter itemB]
        assert_equal 0 [r bf.exists spillfilter neveradded]
    }

    test {BLOOM module: add to filter after spill/fetch round-trip} {
        assert_equal 1 [r bf.add spillfilter itemC]
        assert_equal 1 [r bf.exists spillfilter itemC]
    }

    test {BLOOM module: memory pressure spills filters without crash} {
        # Default-capacity filters are only ~300B; reserve large ones (~25KB
        # bitmap each) so 600 filters (~15MB) exceed the 8MB maxmemory.
        for {set i 0} {$i < 600} {incr i} {
            r bf.reserve bloom:$i 0.01 20000
            r bf.add bloom:$i a$i
        }
        wait_for_counter total_num_items_spilled_to_ext_storage 50 15000
        assert_equal {PONG} [r ping]
    }

    test {BLOOM module: spilled filters answer membership correctly under pressure} {
        foreach i {5 150 300 599} {
            assert_equal 1 [r bf.exists bloom:$i a$i]
            assert_equal 0 [r bf.exists bloom:$i nosuchitem]
        }
    }

    test {BLOOM module: DEL of flash-resident filter does not crash} {
        r bf.add delfilter x
        debug_spill_wait delfilter
        # Regression: used to SIGSEGV in moduleNotifyKeyUnlink (placeholder
        # sds cast to moduleValue*).
        assert_equal 1 [r del delfilter]
        assert_equal {PONG} [r ping]
        assert_equal 0 [r exists delfilter]
    }

    test {BLOOM module: expire of flash-resident filter does not crash} {
        r bf.add expfilter y
        debug_spill_wait expfilter
        r pexpire expfilter 50
        after 200
        assert_equal 0 [r exists expfilter]
        assert_equal {PONG} [r ping]
    }
}
