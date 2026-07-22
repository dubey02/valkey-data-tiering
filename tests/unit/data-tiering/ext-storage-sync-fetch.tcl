# Data tiering: mid-execution synchronous fetch tests.
#
# Covers the extStorageSyncFetch primitive (design-docs/data-tiering/
# sync-fetch-design.md): key accesses that bypass the pre-execution tiering
# filter and previously crashed or returned wrong data:
#   - SORT BY/GET pattern keys (previously SIGSEGV in sortCommandGeneric)
#   - Lua access to keys not declared in KEYS[] (previously read the raw
#     placeholder: strlen returned pointer-digit-count, GET returned the
#     addReply defensive error bytes)
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-sync-fetch

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

# 300-char zero-padded number: numeric for BY scoring, non-embedded so it spills
proc padnum {n} { return [format %0300d $n] }
proc bigval {tag} { return "${tag}[string repeat x 300]" }

start_server [list tags {"ext-storage" "ext-storage-sync-fetch"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache-mock \
    ext-storage-path "/tmp/valkey-flash-sf-[pid].db" \
    ext-storage-capacity-mb 256 \
    maxmemory 50mb \
    maxmemory-policy allkeys-lru \
    enable-debug-command yes \
]] {

    test {SORT BY with flash-resident weight keys does not crash and sorts correctly} {
        r del mylist
        r rpush mylist a b c
        r set weight_a [padnum 3]
        r set weight_b [padnum 1]
        r set weight_c [padnum 2]
        foreach k {weight_a weight_b weight_c} { debug_spill_wait $k }
        assert_equal {b c a} [r sort mylist by weight_* alpha]
        assert_equal {PONG} [r ping]
    }

    test {SORT BY + GET with flash-resident data keys returns fetched values} {
        r set data_a [bigval VA]
        r set data_b [bigval VB]
        r set data_c [bigval VC]
        foreach k {data_a data_b data_c} { debug_spill_wait $k }
        set res [r sort mylist by weight_* get data_* alpha]
        assert_equal 3 [llength $res]
        assert_match {VB*} [lindex $res 0]
        assert_match {VC*} [lindex $res 1]
        assert_match {VA*} [lindex $res 2]
    }

    test {SORT BY hash-field pattern on flash-resident hashes} {
        r del hlist
        r rpush hlist a b c
        # Hashes with a padding field so they are non-embedded spill candidates
        foreach {e w} {a 3 b 1 c 2} {
            r del wh_$e
            r hset wh_$e f [padnum $w] pad [string repeat y 300]
        }
        foreach e {a b c} { debug_spill_wait wh_$e }
        assert_equal {b c a} [r sort hlist by wh_*->f alpha]
    }

    test {Lua undeclared key read returns the real value} {
        r set luakey "[string repeat q 300]"
        debug_spill_wait luakey
        # Previously: strlen returned digit-count of the placeholder pointer
        assert_equal 300 [r eval {return redis.call('strlen', 'luakey')} 0]
        # GET path: previously returned addReply defensive error bytes
        set v [r eval {return redis.call('get', 'luakey2get')} 0]
        assert_equal {} $v ;# absent key -> nil, not garbage
        r set luakey2get "[string repeat q 300]"
        debug_spill_wait luakey2get
        set v [r eval {return redis.call('get', 'luakey2get')} 0]
        assert_equal 300 [string length $v]
        assert_equal [string repeat q 300] $v
    }

    test {Lua undeclared key write on flash-resident value} {
        r set luawr "[string repeat q 300]"
        debug_spill_wait luawr
        assert_equal 303 [r eval {redis.call('append','luawr','XYZ'); return redis.call('strlen','luawr')} 0]
        assert_equal 303 [r strlen luawr]
        assert_match {*XYZ} [r getrange luawr 297 302]
    }

    test {Lua read-your-own-write across a spilled key} {
        r set rywkey "[string repeat q 300]"
        debug_spill_wait rywkey
        set res [r eval {
            redis.call('set', 'rywkey', 'fresh')
            return redis.call('get', 'rywkey')
        } 0]
        assert_equal {fresh} $res
    }

    test {EVAL with undeclared flash key inside MULTI/EXEC} {
        r set txluakey "[string repeat q 300]"
        debug_spill_wait txluakey
        r multi
        r eval {return redis.call('strlen', 'txluakey')} 0
        set res [r exec]
        assert_equal {300} $res
    }

    test {sync fetch metrics are recorded} {
        assert {[get_tiering_counter sync_fetch_count] > 0}
        assert_equal 0 [get_tiering_counter sync_fetch_miss_count]
    }

    test {top-level commands still use the async filter (no sync fetch)} {
        set before [get_tiering_counter sync_fetch_count]
        r set toplevel "[string repeat q 300]"
        debug_spill_wait toplevel
        assert_equal 300 [r strlen toplevel]  ;# blocks + fetches via KBC
        assert_equal $before [get_tiering_counter sync_fetch_count]
    }
}
