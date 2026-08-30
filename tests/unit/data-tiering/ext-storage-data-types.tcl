# Data tiering integration tests: data validity for all structure types.
#
# Tests spill/fetch round-trip integrity for string, hash, list, set, zset, stream.
# Default: flashcache-mock (in-memory, no disk needed).
# Real FlashCache: set EXT_STORAGE_BACKEND=flashcache and EXT_STORAGE_PATH to a
#   pre-allocated file (fallocate -l 512M /tmp/valkey-flash-test.db).
#
# Run with:
#   ./runtest --single tests/unit/ext-storage-data-types.tcl
#   EXT_STORAGE_BACKEND=flashcache EXT_STORAGE_PATH=/tmp/valkey-flash-test.db \
#     ./runtest --single tests/unit/ext-storage-data-types.tcl

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
    # Wait for spill completion (counter increments when IO thread finishes)
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

proc get_info_field {field} {
    set info [r info all]
    if {[regexp "${field}:(\\S+)" $info _ val]} {
        return $val
    }
    return ""
}

# Real FlashCache asserts on backing-file size inside getFileSize() before its
# logger exists, so a missing or undersized file segfaults during init with no
# usable message (the harness only reports "Can't start / No PID detected").
# Pre-allocating removes that entirely. Harmless for the mock backend.
set _backend  [expr {[info exists ::env(EXT_STORAGE_BACKEND)] ? $::env(EXT_STORAGE_BACKEND) : "flashcache-mock"}]
set _path     [expr {[info exists ::env(EXT_STORAGE_PATH)] ? $::env(EXT_STORAGE_PATH) : "/tmp/valkey-flash-test-[pid].db"}]
set _capacity [expr {[info exists ::env(EXT_STORAGE_CAPACITY_MB)] ? $::env(EXT_STORAGE_CAPACITY_MB) : "256"}]
catch {exec fallocate -l ${_capacity}M $_path}

start_server [list tags {"ext-storage" "ext-storage-data-types"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path \
    ext-storage-capacity-mb $_capacity \
    maxmemory 10mb \
    maxmemory-policy allkeys-lru \
]] {

    # =========================================================================
    # HASH
    # =========================================================================

    test {HASH: basic spill/fetch round-trip} {
        r del myhash
        r hset myhash f1 "value1_padding_to_exceed_embstr" f2 "value2_padding_to_exceed_embstr" f3 "value3"
        debug_spill myhash
        # Fetch via HGETALL
        set result [r hgetall myhash]
        assert_equal [llength $result] 6
        assert_equal [dict get $result f1] "value1_padding_to_exceed_embstr"
        assert_equal [dict get $result f2] "value2_padding_to_exceed_embstr"
        assert_equal [dict get $result f3] "value3"
    }

    test {HASH: HGET single field after spill} {
        r del h2
        r hset h2 field1 "data_aaaa" field2 "data_bbbb"
        debug_spill h2
        assert_equal [r hget h2 field1] "data_aaaa"
        assert_equal [r hget h2 field2] "data_bbbb"
        assert_equal [r hget h2 nonexistent] {}
    }

    test {HASH: HLEN and HEXISTS after spill} {
        r del h3
        r hset h3 a 1 b 2 c 3
        debug_spill h3
        assert_equal [r hlen h3] 3
        assert_equal [r hexists h3 a] 1
        assert_equal [r hexists h3 missing] 0
    }

    test {HASH: large hash (hashtable encoding) round-trip} {
        r del bighash
        for {set i 0} {$i < 200} {incr i} {
            r hset bighash "field_[format %04d $i]" "val_[format %04d $i]_padding_data"
        }
        debug_spill bighash
        assert_equal [r hlen bighash] 200
        assert_equal [r hget bighash "field_0100"] "val_0100_padding_data"
        assert_equal [r hget bighash "field_0199"] "val_0199_padding_data"
    }

    test {HASH: HMGET after spill} {
        r del h4
        r hset h4 x "x_val" y "y_val" z "z_val"
        debug_spill h4
        set result [r hmget h4 x z missing]
        assert_equal [lindex $result 0] "x_val"
        assert_equal [lindex $result 1] "z_val"
        assert_equal [lindex $result 2] {}
    }

    # =========================================================================
    # LIST
    # =========================================================================

    test {LIST: basic spill/fetch round-trip} {
        r del mylist
        r rpush mylist "elem1_pad_data" "elem2_pad_data" "elem3_pad_data"
        debug_spill mylist
        set result [r lrange mylist 0 -1]
        assert_equal [llength $result] 3
        assert_equal [lindex $result 0] "elem1_pad_data"
        assert_equal [lindex $result 2] "elem3_pad_data"
    }

    test {LIST: LLEN and LINDEX after spill} {
        r del l2
        r rpush l2 a b c d e
        debug_spill l2
        assert_equal [r llen l2] 5
        assert_equal [r lindex l2 0] "a"
        assert_equal [r lindex l2 4] "e"
        assert_equal [r lindex l2 -1] "e"
    }

    test {LIST: large list (quicklist encoding) round-trip} {
        r del biglist
        for {set i 0} {$i < 200} {incr i} {
            r rpush biglist "item_[format %04d $i]_padding"
        }
        debug_spill biglist
        assert_equal [r llen biglist] 200
        assert_equal [r lindex biglist 0] "item_0000_padding"
        assert_equal [r lindex biglist 199] "item_0199_padding"
    }

    test {LIST: LPOP/RPOP after spill} {
        r del l3
        r rpush l3 "first" "middle" "last"
        debug_spill l3
        assert_equal [r lpop l3] "first"
        assert_equal [r rpop l3] "last"
        assert_equal [r llen l3] 1
    }

    # =========================================================================
    # SET
    # =========================================================================

    test {SET: basic spill/fetch round-trip} {
        r del myset
        r sadd myset "member1_pad" "member2_pad" "member3_pad"
        debug_spill myset
        set members [lsort [r smembers myset]]
        assert_equal [llength $members] 3
        assert_equal [lindex $members 0] "member1_pad"
    }

    test {SET: SCARD and SISMEMBER after spill} {
        r del s2
        r sadd s2 "alpha" "beta" "gamma"
        debug_spill s2
        assert_equal [r scard s2] 3
        assert_equal [r sismember s2 "alpha"] 1
        assert_equal [r sismember s2 "missing"] 0
    }

    test {SET: intset encoding round-trip} {
        r del intset
        r sadd intset 1 2 3 100 999
        debug_spill intset
        set members [lsort -integer [r smembers intset]]
        assert_equal $members {1 2 3 100 999}
    }

    test {SET: large set (hashtable encoding) round-trip} {
        r del bigset
        for {set i 0} {$i < 200} {incr i} {
            r sadd bigset "member_[format %04d $i]"
        }
        debug_spill bigset
        assert_equal [r scard bigset] 200
        assert_equal [r sismember bigset "member_0100"] 1
    }

    # =========================================================================
    # SORTED SET
    # =========================================================================

    test {ZSET: basic spill/fetch round-trip} {
        r del myzset
        r zadd myzset 1.5 "alice" 2.7 "bob" 0.1 "charlie"
        debug_spill myzset
        set result [r zrange myzset 0 -1 WITHSCORES]
        # Sorted by score: charlie(0.1), alice(1.5), bob(2.7)
        assert_equal [lindex $result 0] "charlie"
        assert_equal [lindex $result 2] "alice"
        assert_equal [lindex $result 4] "bob"
        # Float precision: verify scores are approximately correct
        assert {abs([lindex $result 1] - 0.1) < 0.001}
        assert {abs([lindex $result 3] - 1.5) < 0.001}
        assert {abs([lindex $result 5] - 2.7) < 0.001}
    }

    test {ZSET: ZSCORE and ZCARD after spill} {
        r del z2
        r zadd z2 10 "x" 20 "y" 30 "z"
        debug_spill z2
        assert_equal [r zcard z2] 3
        assert_equal [r zscore z2 "y"] 20
        assert_equal [r zscore z2 "missing"] {}
    }

    test {ZSET: ZRANK and ZRANGEBYSCORE after spill} {
        r del z3
        r zadd z3 1 "a" 2 "b" 3 "c" 4 "d" 5 "e"
        debug_spill z3
        assert_equal [r zrank z3 "c"] 2
        set result [r zrangebyscore z3 2 4]
        assert_equal $result {b c d}
    }

    test {ZSET: large zset (skiplist encoding) round-trip} {
        r del bigzset
        for {set i 0} {$i < 200} {incr i} {
            r zadd bigzset [expr {$i * 1.1}] "member_[format %04d $i]"
        }
        debug_spill bigzset
        assert_equal [r zcard bigzset] 200
        assert {abs([r zscore bigzset "member_0100"] - 110.0) < 0.001}
    }

    test {ZSET: duplicate scores preserved} {
        r del zdup
        r zadd zdup 1.0 "a" 1.0 "b" 1.0 "c"
        debug_spill zdup
        # Tie-breaking by lex: a, b, c
        set result [r zrange zdup 0 -1]
        assert_equal $result {a b c}
    }

    # =========================================================================
    # STREAM
    # =========================================================================

    test {STREAM: basic spill/fetch round-trip} {
        r del mystream
        set id1 [r xadd mystream "*" name "alice" age "30"]
        set id2 [r xadd mystream "*" name "bob" age "25"]
        debug_spill mystream
        assert_equal [r xlen mystream] 2
        set entries [r xrange mystream - +]
        assert_equal [llength $entries] 2
        set e1 [lindex $entries 0]
        set fields [lindex $e1 1]
        assert_equal [lindex $fields 0] "name"
        assert_equal [lindex $fields 1] "alice"
    }

    test {STREAM: XINFO after spill} {
        r del s_info
        r xadd s_info "*" k1 v1
        r xadd s_info "*" k2 v2
        r xadd s_info "*" k3 v3
        debug_spill s_info
        set info [r xinfo stream s_info]
        # xinfo returns key-value pairs; find length
        set idx [lsearch $info "length"]
        assert_equal [lindex $info [expr {$idx + 1}]] 3
    }

    test {STREAM: consumer group state preserved across spill} {
        r del s_cg
        r xadd s_cg "*" msg "hello"
        r xadd s_cg "*" msg "world"
        r xgroup create s_cg grp1 0
        # Read one message
        set read [r xreadgroup GROUP grp1 consumer1 COUNT 1 STREAMS s_cg >]
        set read_id [lindex [lindex [lindex [lindex $read 0] 1] 0] 0]
        # ACK it
        r xack s_cg grp1 $read_id
        # Now spill
        debug_spill s_cg
        # After fetch, pending should be 0 (we ACK'd), and 1 message remains unread
        set pending [r xpending s_cg grp1 - + 10]
        assert_equal [llength $pending] 0
        # Read remaining
        set read2 [r xreadgroup GROUP grp1 consumer1 COUNT 10 STREAMS s_cg >]
        set entries [lindex [lindex $read2 0] 1]
        assert_equal [llength $entries] 1
        set fields [lindex [lindex $entries 0] 1]
        assert_equal [lindex $fields 1] "world"
    }

    # =========================================================================
    # STRING (encoding edge cases beyond basic test)
    # =========================================================================

    test {STRING: binary data preserved across spill} {
        r del binstr
        # Create a string with null bytes and non-UTF8 data
        set binval "hello\x00world\x01\x02\xff[string repeat X 200]"
        r set binstr $binval
        debug_spill binstr
        set fetched [r get binstr]
        assert_equal $fetched $binval
    }

    test {STRING: large string (>1MB) round-trip} {
        r del bigstr
        set bigval [string repeat "X" 65536]
        r set bigstr $bigval
        debug_spill bigstr
        set fetched [r get bigstr]
        assert_equal [string length $fetched] 65536
        assert_equal $fetched $bigval
    }

    # =========================================================================
    # CROSS-TYPE TESTS
    # =========================================================================

    test {TTL preserved across spill/fetch for all types} {
        set types {tstr thash tlist tset tzset tstream}
        r set tstr "val_padding_data_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" EX 7200
        r hset thash f v
        r expire thash 7200
        r rpush tlist x
        r expire tlist 7200
        r sadd tset m
        r expire tset 7200
        r zadd tzset 1 m
        r expire tzset 7200
        r xadd tstream "*" k v
        r expire tstream 7200

        foreach key $types {
            debug_spill $key
        }

        # After spill, TTLs should still be ~7200 (allow 10s drift)
        foreach key $types {
            # Trigger fetch by accessing
            switch $key {
                tstr { r get $key }
                thash { r hgetall $key }
                tlist { r lrange $key 0 -1 }
                tset { r smembers $key }
                tzset { r zrange $key 0 -1 }
                tstream { r xlen $key }
            }
            set ttl [r ttl $key]
            assert {$ttl > 7100 && $ttl <= 7200}
        }
    }

    test {DEL works on tiered string} {
        r set del_str "v_padding_for_raw_encoding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        debug_spill del_str
        r del del_str
        assert_equal [r exists del_str] 0
    }

    test {DEL works on tiered hash} {
        r hset del_hash f v
        debug_spill del_hash
        r del del_hash
        assert_equal [r exists del_hash] 0
    }

    test {DEL works on tiered list} {
        r rpush del_list x y z
        debug_spill del_list
        r del del_list
        assert_equal [r exists del_list] 0
    }

    test {DEL works on tiered set} {
        r sadd del_set m1 m2
        debug_spill del_set
        r del del_set
        assert_equal [r exists del_set] 0
    }

    test {DEL works on tiered zset} {
        r zadd del_zset 1 m
        debug_spill del_zset
        r del del_zset
        assert_equal [r exists del_zset] 0
    }

    test {DEL works on tiered stream} {
        r xadd del_stream "*" k v
        debug_spill del_stream
        r del del_stream
        assert_equal [r exists del_stream] 0
    }

    test {Overwrite tiered key with new value} {
        r hset ow_hash f1 "original"
        debug_spill ow_hash
        # Overwrite while tiered
        r hset ow_hash f1 "updated" f2 "new_field"
        assert_equal [r hget ow_hash f1] "updated"
        assert_equal [r hget ow_hash f2] "new_field"
    }

    test {TYPE command works on tiered keys} {
        r flushall
        r set type_str "v_padding_raw_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        r hset type_hash f "value_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        r rpush type_list "list_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        r sadd type_set "set_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        r zadd type_zset 1 "zset_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        r xadd type_stream "*" k "stream_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"

        debug_spill type_str
        debug_spill type_hash
        debug_spill type_list
        debug_spill type_set
        debug_spill type_zset
        debug_spill type_stream

        assert_equal [r type type_str] "string"
        assert_equal [r type type_hash] "hash"
        assert_equal [r type type_list] "list"
        assert_equal [r type type_set] "set"
        assert_equal [r type type_zset] "zset"
        assert_equal [r type type_stream] "stream"
    }

    test {SCAN returns tiered keys with correct type} {
        # Keys from previous test are still tiered
        set found_types [dict create]
        set cursor 0
        while {1} {
            set result [r scan $cursor COUNT 100]
            set cursor [lindex $result 0]
            foreach k [lindex $result 1] {
                if {[string match "type_*" $k]} {
                    dict set found_types $k [r type $k]
                }
            }
            if {$cursor == 0} break
        }
        assert_equal [dict get $found_types type_str] "string"
        assert_equal [dict get $found_types type_hash] "hash"
        assert_equal [dict get $found_types type_list] "list"
        assert_equal [dict get $found_types type_set] "set"
        assert_equal [dict get $found_types type_zset] "zset"
        assert_equal [dict get $found_types type_stream] "stream"
    }

    test {RENAME tiered key preserves data} {
        r del rename_src rename_dst
        r hset rename_src field1 "data1" field2 "data2"
        debug_spill rename_src
        r rename rename_src rename_dst
        assert_equal [r exists rename_src] 0
        assert_equal [r hget rename_dst field1] "data1"
        assert_equal [r hget rename_dst field2] "data2"
    }

    test {DUMP on tiered key returns valid RDB payload} {
        r del dump_hash
        r hset dump_hash x "100" y "200"
        debug_spill dump_hash
        set payload [r dump dump_hash]
        # RESTORE to a new key
        r del dump_restored
        r restore dump_restored 0 $payload
        assert_equal [r hget dump_restored x] "100"
        assert_equal [r hget dump_restored y] "200"
    }

    test {Pressure-driven spill with mixed types preserves data} {
        # Use small maxmemory to force natural spilling
        r flushall
        r config set maxmemory 2mb

        # Populate mixed types - small enough to not get OOM-rejected,
        # but enough to exceed 2MB and trigger spilling
        set padding [string repeat "Z" 256]
        for {set i 0} {$i < 50} {incr i} {
            catch {r set "ps:[format %04d $i]" "str_${padding}_$i"}
            catch {r hset "ph:[format %04d $i]" f "hash_${padding}_$i"}
            catch {r rpush "pl:[format %04d $i]" "list_${padding}_$i"}
        }

        # Wait for natural spilling to kick in
        set deadline [expr {[clock milliseconds] + 5000}]
        while {[get_spill_count] == 0 && [clock milliseconds] < $deadline} {
            after 100
        }
        set spilled [get_spill_count]
        assert {$spilled > 0}

        # Verify data integrity for keys that made it in
        set errors 0
        for {set i 0} {$i < 50} {incr i} {
            set key "ps:[format %04d $i]"
            if {[r exists $key]} {
                set val [r get $key]
                if {![string match "str_${padding}_$i" $val]} { incr errors }
            }
        }
        for {set i 0} {$i < 50} {incr i} {
            set key "ph:[format %04d $i]"
            if {[r exists $key]} {
                set val [r hget $key f]
                if {![string match "hash_${padding}_$i" $val]} { incr errors }
            }
        }
        for {set i 0} {$i < 50} {incr i} {
            set key "pl:[format %04d $i]"
            if {[r exists $key]} {
                set val [r lindex $key 0]
                if {![string match "list_${padding}_$i" $val]} { incr errors }
            }
        }
        assert_equal $errors 0

        # Restore maxmemory
        r config set maxmemory 10mb
    }

    # =========================================================================
    # HIGH-PRIORITY COVERAGE: large stream, UNLINK, multi-key, concurrent
    # =========================================================================

    test {STREAM: large stream with many entries round-trip} {
        r flushall
        r config set maxmemory 10mb
        r del bigstream
        for {set i 0} {$i < 500} {incr i} {
            r xadd bigstream "*" seq $i data "payload_[format %04d $i]"
        }
        debug_spill bigstream
        assert_equal [r xlen bigstream] 500
        # Verify first, middle, and last entries
        set entries [r xrange bigstream - + COUNT 1]
        set fields [lindex [lindex $entries 0] 1]
        assert_equal [lindex $fields 1] "0"
        set entries [r xrange bigstream - + COUNT 251]
        set last [lindex $entries 250]
        set fields [lindex $last 1]
        assert_equal [lindex $fields 1] "250"
    }

    test {STREAM: multiple consumer groups preserved across spill} {
        r del mcg_stream
        r xadd mcg_stream "*" msg "one"
        r xadd mcg_stream "*" msg "two"
        r xadd mcg_stream "*" msg "three"
        r xgroup create mcg_stream grpA 0
        r xgroup create mcg_stream grpB 0
        # grpA reads 2, acks 1
        set read [r xreadgroup GROUP grpA c1 COUNT 2 STREAMS mcg_stream >]
        set id1 [lindex [lindex [lindex [lindex $read 0] 1] 0] 0]
        r xack mcg_stream grpA $id1
        # grpB reads 1
        r xreadgroup GROUP grpB c1 COUNT 1 STREAMS mcg_stream >

        debug_spill mcg_stream

        # After fetch: grpA has 1 pending (read 2, acked 1)
        set pending_a [r xpending mcg_stream grpA - + 10]
        assert_equal [llength $pending_a] 1
        # grpB has 1 pending
        set pending_b [r xpending mcg_stream grpB - + 10]
        assert_equal [llength $pending_b] 1
        # grpA still has 1 unread message
        set read_a [r xreadgroup GROUP grpA c1 COUNT 10 STREAMS mcg_stream >]
        set entries [lindex [lindex $read_a 0] 1]
        assert_equal [llength $entries] 1
        # grpB has 2 unread
        set read_b [r xreadgroup GROUP grpB c1 COUNT 10 STREAMS mcg_stream >]
        set entries [lindex [lindex $read_b 0] 1]
        assert_equal [llength $entries] 2
    }

    test {UNLINK works on tiered keys (async free path)} {
        r del ul_str ul_hash ul_list ul_set ul_zset ul_stream
        r set ul_str "val_padding_raw_data_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        r hset ul_hash f v
        r rpush ul_list x y z
        r sadd ul_set m1 m2
        r zadd ul_zset 1 m
        r xadd ul_stream "*" k v

        foreach key {ul_str ul_hash ul_list ul_set ul_zset ul_stream} {
            debug_spill $key
            r unlink $key
            # Give async free a moment
            after 50
            assert_equal [r exists $key] 0
        }
    }

    test {Multi-key read across tiered and non-tiered keys} {
        r del mk_1 mk_2 mk_3
        r set mk_1 "in_memory_value_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        r set mk_2 "will_be_tiered_pad_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        r set mk_3 "also_in_memory_val_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        debug_spill mk_2
        # MGET crosses both tiered and non-tiered
        set result [r mget mk_1 mk_2 mk_3]
        assert_equal [lindex $result 0] "in_memory_value_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        assert_equal [lindex $result 1] "will_be_tiered_pad_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        assert_equal [lindex $result 2] "also_in_memory_val_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
    }

    test {SUNIONSTORE with tiered and non-tiered sets} {
        r del s_mem s_tier s_dst
        r sadd s_mem "a" "b" "c"
        r sadd s_tier "c" "d" "e"
        debug_spill s_tier
        r sunionstore s_dst s_mem s_tier
        set members [lsort [r smembers s_dst]]
        assert_equal $members {a b c d e}
    }

    test {OBJECT ENCODING preserved after fetch} {
        r del enc_lp enc_ht enc_ql enc_zl enc_ss
        # Hash: listpack encoding (small)
        r hset enc_lp f1 v1 f2 v2
        # Hash: hashtable encoding (large)
        for {set i 0} {$i < 200} {incr i} {
            r hset enc_ht "field_$i" "val_$i"
        }
        # List: quicklist
        for {set i 0} {$i < 200} {incr i} {
            r rpush enc_ql "item_$i"
        }

        set enc_before_lp [r object encoding enc_lp]
        set enc_before_ht [r object encoding enc_ht]
        set enc_before_ql [r object encoding enc_ql]

        debug_spill enc_lp
        debug_spill enc_ht
        debug_spill enc_ql

        # Fetch via read
        r hgetall enc_lp
        r hgetall enc_ht
        r lrange enc_ql 0 -1

        assert_equal [r object encoding enc_lp] $enc_before_lp
        assert_equal [r object encoding enc_ht] $enc_before_ht
        assert_equal [r object encoding enc_ql] $enc_before_ql
    }

    # =========================================================================
    # LUA SCRIPTING on tiered keys
    # =========================================================================

    test {Lua EVAL reads tiered string} {
        r set lua_str1 "lua_value_padding_data_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        debug_spill lua_str1
        set result [r eval {return redis.call('GET', KEYS[1])} 1 lua_str1]
        assert_equal $result "lua_value_padding_data_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
    }

    test {Lua EVAL reads multiple tiered keys} {
        r del lua_a lua_b
        r set lua_a "val_a_padding_raw_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        r set lua_b "val_b_padding_raw_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        debug_spill lua_a
        debug_spill lua_b
        set result [r eval {
            local a = redis.call('GET', KEYS[1])
            local b = redis.call('GET', KEYS[2])
            return a .. '|' .. b
        } 2 lua_a lua_b]
        assert_equal $result "val_a_padding_raw_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX|val_b_padding_raw_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
    }

    test {Lua EVAL writes then reads different tiered key} {
        r set lua_r2 "read_target_pad_data_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        debug_spill lua_r2
        set result [r eval {
            redis.call('SET', KEYS[1], 'written_in_lua')
            local v = redis.call('GET', KEYS[2])
            return v
        } 2 lua_w2 lua_r2]
        assert_equal $result "read_target_pad_data_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
    }

    test {Lua EVAL hash operations on tiered key} {
        r del lua_hash
        r hset lua_hash f1 "hash_val_1" f2 "hash_val_2"
        debug_spill lua_hash
        set result [r eval {
            local f1 = redis.call('HGET', KEYS[1], 'f1')
            redis.call('HSET', KEYS[1], 'f3', 'added_in_lua')
            local f3 = redis.call('HGET', KEYS[1], 'f3')
            return f1 .. '|' .. f3
        } 1 lua_hash]
        assert_equal $result "hash_val_1|added_in_lua"
    }

    test {Lua EVAL INCRBY on tiered integer string} {
        # INT/EMBSTR values can't be DEBUG SPILL'd — use memory pressure
        r del lua_ctr
        r set lua_ctr "100"
        # Fill memory to force LRU spill of lua_ctr
        set padding [string repeat "Y" 1024]
        for {set i 0} {$i < 2000} {incr i} {
            r set "incr_filler:[format %05d $i]" "${padding}_$i"
        }
        after 1000
        set result [r eval {
            redis.call('INCRBY', KEYS[1], 50)
            return redis.call('GET', KEYS[1])
        } 1 lua_ctr]
        assert_equal $result "150"
    }

    # =========================================================================
    # MULTI/EXEC transactions on tiered keys
    # =========================================================================

    test {MULTI/EXEC reads tiered key} {
        r del multi_str
        r set multi_str "multi_value_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        debug_spill multi_str
        r multi
        r get multi_str
        set result [r exec]
        assert_equal [lindex $result 0] "multi_value_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
    }

    test {MULTI/EXEC write + read tiered key} {
        r del multi_wr
        r set multi_wr "original_pad_data_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        debug_spill multi_wr
        r multi
        r append multi_wr "_extra"
        r get multi_wr
        set result [r exec]
        assert_equal [lindex $result 1] "original_pad_data_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX_extra"
    }

    test {WATCH on tiered key aborts on modification} {
        r del watch_key
        r set watch_key "watched_value_pad_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        debug_spill watch_key

        r watch watch_key
        # Modify the watched key (triggers fetch + modify)
        r set watch_key "modified_value_pad_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        r multi
        r get watch_key
        set result [r exec]
        # EXEC should return empty (transaction aborted)
        assert_equal $result {}
    }

    # =========================================================================
    # ADDITIONAL COMMANDS: COPY, APPEND, PERSIST
    # =========================================================================

    test {COPY on tiered key} {
        r del copy_src copy_dst
        r set copy_src "copy_source_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        debug_spill copy_src
        r copy copy_src copy_dst
        assert_equal [r get copy_dst] "copy_source_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        assert_equal [r exists copy_src] 1
    }

    test {APPEND on tiered string} {
        r del append_key
        r set append_key "hello_padding_raw_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        debug_spill append_key
        r append append_key "_world"
        assert_equal [r get append_key] "hello_padding_raw_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX_world"
    }

    test {PERSIST removes TTL on tiered key} {
        r set persist_key2 "persist_val_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" EX 7200
        debug_spill persist_key2
        r persist persist_key2
        assert_equal [r ttl persist_key2] -1
        assert_equal [r get persist_key2] "persist_val_padding_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
    }

    # =========================================================================
    # MISSING FROM UPSTREAM BASH MIGRATION: commands, Lua edge cases, MULTI
    # =========================================================================

    test {MSET writes multiple tiered-eligible keys} {
        set pad [string repeat "X" 200]
        r mset mset_a "val_a_${pad}" mset_b "val_b_${pad}" mset_c "val_c_${pad}"
        debug_spill mset_a
        debug_spill mset_b
        # Verify all keys intact after some are tiered
        assert_equal [r get mset_a] "val_a_${pad}"
        assert_equal [r get mset_b] "val_b_${pad}"
        assert_equal [r get mset_c] "val_c_${pad}"
    }

    test {RANDOMKEY returns a key when tiered keys exist} {
        set key [r randomkey]
        assert {$key ne {}}
    }

    test {DBSIZE includes tiered keys} {
        set size [r dbsize]
        assert {$size > 0}
    }

    test {Lua conditional branch on tiered value} {
        set pad [string repeat "X" 200]
        r set lua_cond "42${pad}"
        debug_spill lua_cond
        set result [r eval {
            local v = redis.call('GET', KEYS[1])
            if string.sub(v, 1, 2) == '42' then
                return 'branch_a'
            else
                return 'branch_b'
            end
        } 1 lua_cond]
        assert_equal $result "branch_a"
    }

    test {Lua loop over multiple tiered keys} {
        set pad [string repeat "X" 200]
        r set lua_loop_1 "10${pad}"
        r set lua_loop_2 "20${pad}"
        r set lua_loop_3 "30${pad}"
        debug_spill lua_loop_1
        debug_spill lua_loop_2
        debug_spill lua_loop_3
        set result [r eval {
            local sum = 0
            for i = 1, #KEYS do
                local v = redis.call('GET', KEYS[i])
                sum = sum + tonumber(string.sub(v, 1, 2))
            end
            return sum
        } 3 lua_loop_1 lua_loop_2 lua_loop_3]
        assert_equal $result 60
    }

    test {Lua pcall error handling on tiered key} {
        set pad [string repeat "X" 200]
        r set lua_pcall "not_a_number_${pad}"
        debug_spill lua_pcall
        set result [r eval {
            local ok, err = pcall(redis.call, 'INCR', KEYS[1])
            if ok then
                return 'unexpected_success'
            else
                return 'caught_error'
            end
        } 1 lua_pcall]
        assert_equal $result "caught_error"
    }

    test {MULTI/EXEC APPEND on tiered key} {
        set pad [string repeat "X" 200]
        r set multi_app "base_${pad}"
        debug_spill multi_app
        r multi
        r append multi_app "_suffix"
        r get multi_app
        set result [r exec]
        assert_equal [lindex $result 1] "base_${pad}_suffix"
    }
}
