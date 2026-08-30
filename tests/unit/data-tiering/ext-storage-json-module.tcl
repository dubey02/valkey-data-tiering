# Data tiering + JSON module (OBJ_MODULE) compatibility tests.
#
# Requires libjson.so from valkey-io/valkey-json built against this fork's
# valkeymodule.h. Set JSON_MODULE_PATH to the .so; tests are skipped when unset.
#
#   JSON_MODULE_PATH=/tmp/valkey-json/build/src/libjson.so \
#     ./runtest --single unit/data-tiering/ext-storage-json-module
#
# Includes regression coverage for the moduleNotifyKeyUnlink tiered-placeholder
# crash (DEL/expiry of flash-resident module key), fixed by the
# !objectIsTiered() guard in module.c.

if {![info exists ::env(JSON_MODULE_PATH)] || ![file exists $::env(JSON_MODULE_PATH)]} {
    if {$::verbose} { puts "Skipping json-module tiering tests: JSON_MODULE_PATH not set" }
    return
}
set ::json_module_path $::env(JSON_MODULE_PATH)

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
set _fcpath "/tmp/valkey-flash-json-[pid].db"
catch {exec fallocate -l 256M $_fcpath}

start_server [list tags {"ext-storage" "ext-storage-json-module"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache-mock \
    ext-storage-path $_fcpath \
    ext-storage-capacity-mb 256 \
    maxmemory 8mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command yes \
    loadmodule $::json_module_path \
]] {

    test {JSON module: baseline ops with tiering enabled} {
        r json.set basedoc . {{"name":"test","nested":{"arr":[1,2,3]},"num":42}}
        assert_equal {[1,2,3]} [r json.get basedoc .nested.arr]
        assert_equal {ReJSON-RL} [r type basedoc]
    }

    test {JSON module: explicit spill of a module key succeeds} {
        r json.set spilldoc . {{"hello":"world","arr":[1,2,3,4,5]}}
        debug_spill_wait spilldoc
        assert {[get_tiering_counter num_items_on_flash] >= 1}
        # Server must survive the spill (IO-thread DUMP serialization of OBJ_MODULE)
        assert_equal {PONG} [r ping]
    }

    test {JSON module: fetch spilled key round-trip preserves document} {
        # spilldoc is on flash from the previous test; JSON.GET forces a fetch
        assert_equal {{"hello":"world","arr":[1,2,3,4,5]}} [r json.get spilldoc .]
        assert_equal {[1,2,3,4,5]} [r json.get spilldoc .arr]
    }

    test {JSON module: modify document after spill/fetch round-trip} {
        r json.set spilldoc .hello {"modified"}
        assert_equal {"modified"} [r json.get spilldoc .hello]
    }

    test {JSON module: memory pressure spills module keys without crash} {
        set payload [string repeat y 8000]
        for {set i 0} {$i < 1200} {incr i} {
            r json.set jdoc:$i . "{\"id\":$i,\"data\":\"$payload\"}"
        }
        wait_for_counter total_num_items_spilled_to_ext_storage 100 15000
        assert_equal {PONG} [r ping]
    }

    test {JSON module: spilled docs read back intact under pressure} {
        foreach i {5 300 777 1150} {
            assert_equal $i [r json.get jdoc:$i .id]
        }
    }

    test {JSON module: write to a spilled doc works} {
        r json.set jdoc:10 .extra {"added-after-fetch"}
        assert_equal {"added-after-fetch"} [r json.get jdoc:10 .extra]
    }
}

# --------------------------------------------------------------------------
# Regression tests for the moduleNotifyKeyUnlink tiered-placeholder crash:
# DEL/expiry of a flash-resident module key used to SIGSEGV because the
# placeholder sds was cast to moduleValue* (entry->type stays OBJ_MODULE
# after spill). Fixed by the !objectIsTiered() guard in moduleNotifyKeyUnlink.
# --------------------------------------------------------------------------
# Real FlashCache asserts on backing-file size inside getFileSize() before its
# logger exists, so a missing or undersized file segfaults during init with no
# usable message (the harness only reports "Can't start / No PID detected").
# Pre-allocating removes that entirely. Harmless for the mock backend.
set _fcpath2 "/tmp/valkey-flash-json-del-[pid].db"
catch {exec fallocate -l 256M $_fcpath2}

start_server [list tags {"ext-storage" "ext-storage-json-module"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache-mock \
    ext-storage-path $_fcpath2 \
    ext-storage-capacity-mb 256 \
    maxmemory 20mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command yes \
    loadmodule $::json_module_path \
]] {

    test {JSON module: DEL of flash-resident module key does not crash} {
        r json.set deldoc . {{"id":1,"data":"payload-xxxxxxxxxxxxxxxx"}}
        debug_spill_wait deldoc
        assert_equal 1 [r del deldoc]
        assert_equal {PONG} [r ping]
        assert_equal 0 [r exists deldoc]
    }

    test {JSON module: expire of flash-resident module key does not crash} {
        r json.set expdoc . {{"id":2,"data":"payload"}}
        debug_spill_wait expdoc
        r pexpire expdoc 50
        after 200
        # Lazy/active expire of the flash key drives the same unlink path
        assert_equal 0 [r exists expdoc]
        assert_equal {PONG} [r ping]
    }
}
