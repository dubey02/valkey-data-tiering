# Key-spilling integration tests (ext-key-spill-enabled).
#
# A key-spilled key has no dict entry at all: key + serialized value + TTL
# live only on the storage backend. These tests cover demotion
# (DEBUG KEYSPILL / pressure-driven), the dict-miss fetch path, probe
# accounting, DEL reply correctness, and keyspace-visible counters.
#
# Default backend: flashcache-mock (in-memory, no disk needed).
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-key-spilling

proc get_info_field {field} {
    set info [r info all]
    if {[regexp "${field}:(\\S+)" $info _ val]} {
        return $val
    }
    return ""
}

proc get_spill_count {} {
    set v [get_info_field total_num_items_spilled_to_ext_storage]
    if {$v eq ""} { return 0 }
    return $v
}

proc debug_spill {key {timeout 5000}} {
    set before [get_spill_count]
    r debug spill $key
    set start [clock milliseconds]
    while {1} {
        if {[get_spill_count] > $before} { return }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill of '$key'"
        }
        after 50
    }
}

# Spill a key and ensure its dict entry is dropped: the full demotion
# sequence. With together-spill, the WRITE completion drops the entry
# inline, so DEBUG KEYSPILL usually finds it already gone and errors.
# Swallow that error; assert the demotion actually happened either way.
proc key_spill {key} {
    set dropped_before [get_info_field total_keys_dropped_from_dict]
    debug_spill $key
    catch {r debug keyspill $key}
    if {[get_info_field total_keys_dropped_from_dict] <= $dropped_before} {
        error "Key '$key' was not demoted (no dict drop recorded)"
    }
}

start_server [list tags {"ext-storage" "ext-storage-key-spilling"} overrides [list \
    enable-debug-command yes \
    ext-storage-enabled yes \
    ext-storage-backend [expr {[info exists ::env(EXT_STORAGE_BACKEND)] ? $::env(EXT_STORAGE_BACKEND) : "flashcache-mock"}] \
    ext-storage-path [expr {[info exists ::env(EXT_STORAGE_PATH)] ? $::env(EXT_STORAGE_PATH) : "/tmp/valkey-flash-test-[pid].db"}] \
    ext-storage-capacity-mb [expr {[info exists ::env(EXT_STORAGE_CAPACITY_MB)] ? $::env(EXT_STORAGE_CAPACITY_MB) : "256"}] \
    ext-key-spill-enabled yes \
    maxmemory 10mb \
    maxmemory-policy allkeys-lru \
]] {

    set padding [string repeat "x" 300]

    # =========================================================================
    # Demotion (DEBUG KEYSPILL)
    # =========================================================================

    test {KEYSPILL: demotion removes the dict entry, data stays on flash} {
        r flushall
        r set ks:basic "hello_${padding}"
        key_spill ks:basic
        assert_equal [r dbsize] 0
        assert_equal [get_info_field keys_key_spilled] 1
    }

    test {KEYSPILL: GET fetches the key back with the correct value} {
        assert_equal [r get ks:basic] "hello_${padding}"
        assert_equal [r dbsize] 1
        assert_equal [get_info_field keys_key_spilled] 0
    }

    test {KEYSPILL: DEBUG KEYSPILL rejects a DRAM-resident key} {
        r set ks:resident "v_${padding}"
        assert_error "*not droppable*" {r debug keyspill ks:resident}
        r del ks:resident
    }

    test {KEYSPILL: DEBUG KEYSPILL rejects a missing key} {
        assert_error "*not droppable*" {r debug keyspill ks:no-such-key}
    }

    # =========================================================================
    # Dict-miss fetch path
    # =========================================================================

    test {KEYSPILL: EXISTS on a key-spilled key} {
        r flushall
        r set ks:exists "v_${padding}"
        key_spill ks:exists
        assert_equal [r exists ks:exists] 1
    }

    test {KEYSPILL: TYPE on a key-spilled key} {
        r del ks:type
        r rpush ks:type a b c
        key_spill ks:type
        assert_equal [r type ks:type] "list"
    }

    test {KEYSPILL: GET of a never-existed key is a plain miss} {
        r flushall
        r set ks:gate "v_${padding}"
        key_spill ks:gate
        # Gate is open (keys_key_spilled > 0): the miss consults the backend.
        assert_equal [r get ks:never-existed] {}
        # Immediate repeat must also miss (confirmed-absent consumed, no loop).
        assert_equal [r get ks:never-existed] {}
    }

    test {KEYSPILL: probing a never-existed key does not close the gate} {
        # Regression: the probe for ks:never-existed above must NOT have
        # decremented keys_key_spilled, or ks:gate becomes unreachable.
        assert_equal [get_info_field keys_key_spilled] 1
        assert_equal [r get ks:gate] "v_${padding}"
    }

    test {KEYSPILL: DEL of a never-existed key replies 0} {
        r flushall
        r set ks:gate2 "v_${padding}"
        key_spill ks:gate2
        assert_equal [r del ks:del-never-existed] 0
        assert_equal [get_info_field keys_key_spilled] 1
    }

    test {KEYSPILL: DEL of a key-spilled key replies 1 and removes it} {
        assert_equal [r del ks:gate2] 1
        assert_equal [r exists ks:gate2] 0
        assert_equal [r dbsize] 0
    }

    test {KEYSPILL: SET overwrites a key-spilled key} {
        r flushall
        r set ks:ow "old_${padding}"
        key_spill ks:ow
        r set ks:ow "new_value"
        assert_equal [r get ks:ow] "new_value"
        assert_equal [get_info_field keys_key_spilled] 0
    }

    test {KEYSPILL: MULTI/EXEC touching a key-spilled key} {
        r flushall
        r set ks:multi "m_${padding}"
        key_spill ks:multi
        r multi
        r get ks:multi
        r incr ks:counter
        set res [r exec]
        assert_equal [lindex $res 0] "m_${padding}"
        assert_equal [lindex $res 1] 1
    }

    # =========================================================================
    # TTL and types
    # =========================================================================

    test {KEYSPILL: TTL survives demotion and fetch} {
        r flushall
        r set ks:ttl "t_${padding}" ex 3600
        key_spill ks:ttl
        assert_equal [r get ks:ttl] "t_${padding}"
        set ttl [r ttl ks:ttl]
        assert {$ttl > 3500 && $ttl <= 3600}
    }

    test {KEYSPILL: hash round-trip} {
        r del ks:hash
        r hset ks:hash f1 "one_${padding}" f2 "two_${padding}"
        key_spill ks:hash
        assert_equal [r hget ks:hash f1] "one_${padding}"
        assert_equal [r hlen ks:hash] 2
    }

    test {KEYSPILL: zset round-trip} {
        r del ks:zset
        r zadd ks:zset 1 "a_${padding}" 2 "b_${padding}"
        key_spill ks:zset
        assert_equal [r zscore ks:zset "b_${padding}"] 2
        assert_equal [r zcard ks:zset] 2
    }

    # =========================================================================
    # Counters and keyspace ops
    # =========================================================================

    test {KEYSPILL: FLUSHALL resets keys_key_spilled} {
        r flushall
        r set ks:f1 "v_${padding}"
        r set ks:f2 "v_${padding}"
        key_spill ks:f1
        key_spill ks:f2
        assert_equal [get_info_field keys_key_spilled] 2
        r flushall
        assert_equal [get_info_field keys_key_spilled] 0
        assert_equal [r get ks:f1] {}
    }

    test {KEYSPILL: demotion and re-materialization counters advance} {
        r flushall
        set drops_before [get_info_field total_keys_dropped_from_dict]
        set remat_before [get_info_field total_keys_rematerialized]
        r set ks:ctr "v_${padding}"
        key_spill ks:ctr
        assert_equal [r get ks:ctr] "v_${padding}"
        assert {[get_info_field total_keys_dropped_from_dict] == $drops_before + 1}
        assert {[get_info_field total_keys_rematerialized] == $remat_before + 1}
    }

    # =========================================================================
    # Pressure-driven demotion (stage 2 of the spill controller)
    # =========================================================================

    test {KEYSPILL: memory pressure demotes keys automatically} {
        r flushall
        r config set maxmemory 0
        # Write enough data that the dict-side overhead alone matters, then
        # clamp maxmemory below current usage: value spill runs first, and
        # the drop stage must engage to reach the setpoint.
        set big [string repeat "y" 900]
        for {set i 0} {$i < 8000} {incr i} {
            r set "ks:p:[format %05d $i]" "v${i}_${big}"
        }
        set used [get_info_field used_memory]
        assert {$used > 8000000}
        r config set maxmemory 5mb
        # Wait for the drop stage to engage.
        set start [clock milliseconds]
        while {[get_info_field keys_key_spilled] == 0} {
            if {[clock milliseconds] - $start > 30000} {
                error "Timed out waiting for pressure-driven demotion"
            }
            after 100
        }
        # Freeze the system before sampling: with the mock backend spilled
        # bytes stay in used_memory, so pressure never relents and allkeys-lru
        # keeps evicting in a race with demotion. Use a HIGH cap, not 0 —
        # maxmemory 0 disables the beforeSleep pump entirely and strands
        # in-flight completions.
        r config set maxmemory 500mb
        set start [clock milliseconds]
        while {[get_info_field num_items_spilling_to_ext_storage] != 0} {
            if {[clock milliseconds] - $start > 10000} { error "spill drain timeout" }
            after 50
        }
        # Conservation: every written key is in the dict, key-spilled, or was
        # explicitly evicted (counted). Nothing may vanish silently. One INFO
        # snapshot for all three terms.
        set info [r info everything]
        # Tests run in db9, not db0 — sum every dbN keyspace line.
        set dict_keys 0
        foreach {_ n} [regexp -all -inline {db\d+:keys=(\d+)} $info] {
            incr dict_keys $n
        }
        set spilled_keys 0
        regexp {keys_key_spilled:(\d+)} $info _ spilled_keys
        set evicted 0
        regexp {evicted_keys:(\d+)} $info _ evicted
        assert {$spilled_keys > 0}
        assert_equal [expr {$dict_keys + $spilled_keys + $evicted}] 8000
    }

    test {KEYSPILL: no corruption after pressure-driven demotion} {
        # Every key is either exactly right or was evicted (nil) — a wrong
        # value is never acceptable. Cap already at 500mb (frozen above).
        set big [string repeat "y" 900]
        set matched 0
        set evicted 0
        for {set i 0} {$i < 8000} {incr i} {
            set v [r get "ks:p:[format %05d $i]"]
            if {$v eq {}} {
                incr evicted
            } elseif {$v eq "v${i}_${big}"} {
                incr matched
            } else {
                error "Corrupt value for ks:p:[format %05d $i]"
            }
        }
        assert {$matched > 0}
        assert_equal [expr {$matched + $evicted}] 8000
    }

    # =========================================================================
    # Duplicate-fetch suppression + progressive drain
    # =========================================================================

    test {KEYSPILL: concurrent GETs of the same spilled key submit one fetch} {
        r config set maxmemory 10mb
        r flushall
        r set ks:dup "dupval_${padding}"
        key_spill ks:dup
        set miss_before [get_info_field kbc_keyspill_miss_fetch]

        # Hold completions so both clients are provably parked on the key at
        # the same time: the first GET rematerializes the probe and submits;
        # the second must find the fetch in flight and park without a second
        # submit (the probe's fetch-in-flight state is the in-flight signal).
        r debug ext-storage-pause-completions 1
        set rd1 [valkey_deferring_client]
        set rd2 [valkey_deferring_client]
        $rd1 get ks:dup
        $rd2 get ks:dup
        wait_for_blocked_clients_count 2
        r debug ext-storage-pause-completions 0

        assert_equal [$rd1 read] "dupval_${padding}"
        assert_equal [$rd2 read] "dupval_${padding}"
        $rd1 close
        $rd2 close

        # Exactly one dict miss was routed to a flash consult. The second
        # client parked on the in-flight fetch instead of re-submitting.
        assert_equal [expr {[get_info_field kbc_keyspill_miss_fetch] - $miss_before}] 1
        assert_equal [r dbsize] 1
    }

    test {KEYSPILL: multi-key command with expired spilled key does not corrupt neighbors} {
        r flushall
        # Regression for the per-key delete flag: an expired tiered key ahead
        # of a healthy tiered key in one command must not convert the healthy
        # key's READ into a destructive DELETE.
        r set ks:mk:expired "gone_${padding}"
        r pexpire ks:mk:expired 80
        debug_spill ks:mk:expired
        r set ks:mk:alive "alive_${padding}"
        debug_spill ks:mk:alive
        after 150
        assert_equal [r mget ks:mk:expired ks:mk:alive] [list {} "alive_${padding}"]
        assert_equal [r get ks:mk:alive] "alive_${padding}"
    }

    # =========================================================================
    # Together-spill: drop at spill completion
    # =========================================================================

    test {KEYSPILL: value spill alone demotes the key (together-spill)} {
        r config set maxmemory 10mb
        r flushall
        set drops_before [get_info_field total_keys_dropped_at_completion]
        r set ks:tog "together_${padding}"
        debug_spill ks:tog
        # No DEBUG KEYSPILL: the WRITE completion itself dropped the entry.
        assert_equal [r dbsize] 0
        assert_equal [get_info_field keys_key_spilled] 1
        assert_equal [expr {[get_info_field total_keys_dropped_at_completion] - $drops_before}] 1
        assert_equal [r get ks:tog] "together_${padding}"
        assert_equal [r dbsize] 1
    }

    test {KEYSPILL: a waiter blocked during spill keeps the dict entry} {
        r flushall
        set drops_before [get_info_field total_keys_dropped_at_completion]
        r set ks:touch "touched_${padding}"
        # Hold the spill completion in flight, then touch the key with a
        # write. The write parks on the in-flight spill. When the completion
        # lands, the waiter is the signal that the key is not cold: the
        # entry must be kept so the drop does not race the re-execution.
        r debug ext-storage-pause-completions 1
        r debug spill ks:touch
        set rd [valkey_deferring_client]
        $rd set ks:touch "rewritten_${padding}"
        wait_for_blocked_clients_count 1
        r debug ext-storage-pause-completions 0
        assert_equal [$rd read] "OK"
        $rd close
        assert_equal [r get ks:touch] "rewritten_${padding}"
        assert_equal [expr {[get_info_field total_keys_dropped_at_completion] - $drops_before}] 0
        assert_equal [r dbsize] 1
    }

    test {KEYSPILL: server alive after all tests (no crash)} {
        r config set maxmemory 10mb
        r flushall
        assert_equal [r ping] "PONG"
    }
}

# =============================================================================
# Gate disabled: DEBUG KEYSPILL must refuse
# =============================================================================

start_server [list tags {"ext-storage" "ext-storage-key-spilling"} overrides [list \
    enable-debug-command yes \
    ext-storage-enabled yes \
    ext-storage-backend "flashcache-mock" \
    ext-storage-path "/tmp/valkey-flash-test-off-[pid].db" \
    ext-storage-capacity-mb 256 \
    maxmemory 10mb \
    maxmemory-policy allkeys-lru \
]] {
    test {KEYSPILL: DEBUG KEYSPILL errors when ext-key-spill-enabled is off} {
        r set ks:off "value_padding_to_exceed_embstr_limit_aaaaaaaaaaaaaaaa"
        assert_error "*ext-key-spill-enabled*" {r debug keyspill ks:off}
    }

    test {KEYSPILL: config is runtime-modifiable} {
        r config set ext-key-spill-enabled yes
        assert_equal [lindex [r config get ext-key-spill-enabled] 1] "yes"
        r config set ext-key-spill-enabled no
    }
}
