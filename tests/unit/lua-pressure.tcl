proc wait_for_spill {min {timeout 10000}} {
    set start [clock milliseconds]
    while {1} {
        set info [r info all]
        if {[regexp {total_num_items_spilled_to_ext_storage:(\d+)} $info _ val]} {
            if {$val >= $min} { return $val }
        }
        if {[clock milliseconds] - $start > $timeout} { error "spill timeout (got $val)" }
        after 100
    }
}
start_server [list tags {"lua-pressure"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path /tmp/valkey-flash-pressure.db \
    ext-storage-capacity-mb 256 \
    maxmemory 2mb \
    maxmemory-policy allkeys-lru \
]] {
    test {Lua EVAL reads tiered key (memory-pressure spill)} {
        # Write our target key with a large value
        r set lua_target "lua_value_padding_data_that_is_large_enough_to_spill_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
        # Fill memory to force LRU spill
        set padding [string repeat "Y" 1024]
        for {set i 0} {$i < 3000} {incr i} {
            r set "filler:[format %05d $i]" "${padding}_$i"
        }
        # Wait for spills to happen
        wait_for_spill 100
        # Now try Lua EVAL on our target (which should be tiered by now)
        set result [r eval {return redis.call('GET', KEYS[1])} 1 lua_target]
        assert_equal $result "lua_value_padding_data_that_is_large_enough_to_spill_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
    }
}
