# Data tiering persistence tests: AOF, RDB interactions with tiered keys.
#
# Tests that persistence mechanisms (AOF rewrite, RDB save) handle tiered
# objects without crashing, and documents known data-loss limitations.
#
# Run with:
#   ./runtest --single tests/unit/ext-storage-persistence.tcl

proc get_spill_count {} {
    set info [r info all]
    if {[regexp {total_num_items_spilled_to_ext_storage:(\d+)} $info _ val]} {
        return $val
    }
    return 0
}

proc debug_spill {key {timeout 5000}} {
    set before [get_spill_count]
    r debug spill $key
    set start [clock milliseconds]
    while {1} {
        set now [get_spill_count]
        if {$now > $before} {
            return
        }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill of '$key' (count stuck at $now)"
        }
        after 50
    }
}

if {[info exists ::env(EXT_STORAGE_BACKEND)]} {
    set _backend $::env(EXT_STORAGE_BACKEND)
} else {
    set _backend "flashcache-mock"
}
if {[info exists ::env(EXT_STORAGE_PATH)]} {
    set _path $::env(EXT_STORAGE_PATH)
} else {
    set _path "/tmp/valkey-flash-persist-[pid].db"
}
if {[info exists ::env(EXT_STORAGE_CAPACITY_MB)]} {
    set _capacity $::env(EXT_STORAGE_CAPACITY_MB)
} else {
    set _capacity "256"
}

# Real FlashCache asserts on backing-file size inside getFileSize() before its
# logger exists, so a missing or undersized file segfaults during init with no
# usable message (the harness only reports "Can't start / No PID detected").
# Pre-allocating removes that entirely. Harmless for the mock backend.
catch {exec fallocate -l ${_capacity}M $_path}

start_server [list tags {"ext-storage" "ext-storage-persistence"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path \
    ext-storage-capacity-mb $_capacity \
    maxmemory 10mb \
    maxmemory-policy allkeys-lru \
]] {

    test {BGREWRITEAOF does not crash with tiered keys} {
        r flushall
        r config set maxmemory 10mb
        r set aof_str "string_value_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        r hset aof_hash f1 v1 f2 v2
        r rpush aof_list a b c
        r sadd aof_set x y z
        r zadd aof_zset 1 a 2 b
        r xadd aof_stream "*" k v

        debug_spill aof_str
        debug_spill aof_hash
        debug_spill aof_list
        debug_spill aof_set
        debug_spill aof_zset
        debug_spill aof_stream

        r config set appendonly yes
        waitForBgrewriteaof r

        assert_equal [r ping] "PONG"
        assert_equal [r dbsize] 6
    }

    test {AOF rewrite (RDB preamble) preserves tiered keys} {
        # The RDB-preamble base of the AOF rewrite serializes flash-resident
        # values through the snapshot materialization path (fork-based, see
        # ext-storage-snapshot.tcl), so tiered keys now SURVIVE an AOF
        # reload. (The non-preamble rewrite path still skips them with a
        # warning; aof-use-rdb-preamble defaults to yes.)
        r flushall
        r config set maxmemory 10mb
        r config set appendonly yes
        waitForBgrewriteaof r

        r set persist_str "important_data_padding_[string repeat X 200]"
        r hset persist_hash f1 val1 f2 val2
        r rpush persist_list x y z

        debug_spill persist_str
        debug_spill persist_hash
        debug_spill persist_list

        r bgrewriteaof
        waitForBgrewriteaof r

        r debug loadaof

        assert_equal 3 [r dbsize]
        assert_equal "important_data_padding_[string repeat X 200]" [r get persist_str]
        assert_equal "val1" [r hget persist_hash f1]
        assert_equal {x y z} [r lrange persist_list 0 -1]
        r config set appendonly no
    }
}
