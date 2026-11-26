# Stays 0 when a tag filter skips the block that checks.
set tiering_built 0
start_server {tags {"ext-storage"}} {
    # INFO ext_storage is empty on a server built without data tiering.
    set tiering_built [string match {*ext_storage_enabled:*} [r info ext_storage]]
    if {$tiering_built} {
        test {DEBUG OBJECT reports storage_state for in-memory keys} {
            r flushdb
            r set str v
            r rpush list a b
            r hset hash f v
            foreach key {str list hash} {
                assert_match {* storage_state:0 *} [r debug object $key]
            }
        } {} {needs:debug}

        test {DEBUG EXT-STORAGE SPILL needs data tiering enabled} {
            r set big [string repeat x 1000]
            assert_error {*not enabled*} {r debug ext-storage spill big}
        } {} {needs:debug external:skip}
        test {DEBUG EXT-STORAGE is refused while data tiering is off} {
            assert_error {*ext-storage is not enabled*} {r debug ext-storage pause-foreground 1}
        } {} {needs:debug}

        # FlashCache keeps its file in each test valkey server's own directory.
        set engines {
            mock {ext-storage-enabled yes ext-storage-engine mock}
            flashcache {ext-storage-enabled yes ext-storage-engine flashcache ext-storage-path fc ext-storage-capacity 64mb}
        }
        foreach {engine overrides} $engines {
            start_server [list overrides $overrides] {
                test "DEBUG EXT-STORAGE PAUSE-FOREGROUND sets and clears the pause on $engine" {
                    assert_equal 0 [getInfoProperty [r info ext_storage] ext_storage_foreground_paused]
                    assert_equal OK [r debug ext-storage pause-foreground 1]
                    assert_equal 1 [getInfoProperty [r info ext_storage] ext_storage_foreground_paused]
                    assert_equal OK [r debug ext-storage pause-foreground 0]
                    assert_equal 0 [getInfoProperty [r info ext_storage] ext_storage_foreground_paused]
                } {} {needs:debug}

                test "DEBUG EXT-STORAGE PAUSE-BACKGROUND sets and clears the pause on $engine" {
                    assert_equal 0 [getInfoProperty [r info ext_storage] ext_storage_background_paused]
                    assert_equal OK [r debug ext-storage pause-background 1]
                    assert_equal 1 [getInfoProperty [r info ext_storage] ext_storage_background_paused]
                    assert_equal OK [r debug ext-storage pause-background 0]
                    assert_equal 0 [getInfoProperty [r info ext_storage] ext_storage_background_paused]
                } {} {needs:debug}

                test "DEBUG EXT-STORAGE rejects bad arguments and keeps the state on $engine" {
                    assert_error {*Value must be 0 or 1*} {r debug ext-storage pause-foreground 2}
                    assert_error {*not an integer*} {r debug ext-storage pause-background yes}
                    assert_error {*unknown subcommand*} {r debug ext-storage pause-everything 1}
                    assert_error {*unknown subcommand*} {r debug ext-storage pause-foreground}
                    assert_equal 0 [getInfoProperty [r info ext_storage] ext_storage_foreground_paused]
                    assert_equal 0 [getInfoProperty [r info ext_storage] ext_storage_background_paused]
                } {} {needs:debug}

                test "Commands on in-memory keys still run while both sides are paused on $engine" {
                    r debug ext-storage pause-foreground 1
                    r debug ext-storage pause-background 1
                    r set k v
                    set got [r get k]
                    r debug ext-storage pause-foreground 0
                    r debug ext-storage pause-background 0
                    assert_equal v $got
                } {} {needs:debug}

                test "A restart while paused shuts down cleanly and comes back unpaused on $engine" {
                    r debug ext-storage pause-foreground 1
                    r debug ext-storage pause-background 1
                    restart_server 0 true false
                    verify_log_message 0 "*Background processing*paused by DEBUG*" 0
                    verify_log_message 0 "*ready to exit, bye bye*" 0
                    assert_equal 0 [getInfoProperty [r info ext_storage] ext_storage_foreground_paused]
                    assert_equal 0 [getInfoProperty [r info ext_storage] ext_storage_background_paused]
                } {} {needs:debug external:skip}
            }
        }
    }
}

# A storage engine may finish a spill after DEBUG EXT-STORAGE SPILL returns, so
# this waits until each key's value is on flash.
proc wait_for_flash {args} {
    foreach key $args {
        wait_for_condition 100 50 {
            [string match {* storage_state:2 *} [r debug object $key fast]]
        } else {
            puts [r info ext_storage]
            puts [exec tail -n 20 < [srv 0 stdout]]
            fail "$key didn't reach flash"
        }
    }
}

# Pauses the storage engine and submits spills until the queue refuses one, so
# the next fetch has no slot. The pause stays on until the caller lifts it.
proc fill_storage_queue {} {
    r debug ext-storage pause-background 1
    set batch 0
    while {1} {
        set keys {}
        for {set i 0} {$i < 256} {incr i} {
            set k "fill:$batch:$i"
            r set $k [string repeat y 1000]
            lappend keys $k
        }
        if {[catch {r debug ext-storage spill {*}$keys} err]} {
            assert_match {*Spilling the key*failed*} $err
            break
        }
        incr batch
        assert {$batch < 100}
    }
}

# Sends GET $key from a new deferring client and waits until its fetch is on the
# deferred list. Returns the client.
proc start_deferred_get {key} {
    set rd [valkey_deferring_client]
    $rd get $key
    wait_for_condition 50 100 {
        [getInfoProperty [r info ext_storage] ext_storage_num_fetches_deferred] >= 1
    } else {
        fail "the fetch was not deferred"
    }
    return $rd
}

# The value of the INFO ext_storage field ext_storage_<name>.
proc ext_storage_info {name} {
    return [getInfoProperty [r info ext_storage] ext_storage_$name]
}

proc fetched_keys {} {
    return [ext_storage_info total_num_items_fetched_from_storage]
}

# Prints the storage state and the end of the log of valkey server 'level',
# then fails.
proc fail_with_state {msg {level 0}} {
    puts [r $level info ext_storage]
    puts [exec tail -n 20 < [srv $level stdout]]
    fail $msg
}

proc spill_and_wait {args} {
    assert_equal OK [r debug ext-storage spill {*}$args]
    wait_for_flash {*}$args
}

proc wait_for_blocked {count} {
    wait_for_condition 50 100 {
        [getInfoProperty [r info clients] blocked_clients] == $count
    } else {
        fail_with_state "$count clients weren't blocked"
    }
}

# Creates one spillable key of each type and encoding in the selected database,
# and returns a dict of each key and its encoding. No key has a TTL, and every
# string is too long to be embedded, so each one can be spilled.
proc build_every_type {} {
    set bytes {}
    for {set i 0} {$i < 256} {incr i} {
        lappend bytes $i
    }
    r set string:binary [string repeat [binary format c* $bytes] 4]
    r set string:large [string repeat l [expr {1024 * 1024 + 1}]]
    r rpush list:listpack a b c
    # Over list-max-listpack-size.
    r rpush list:quicklist [string repeat q 10000] b
    r sadd set:intset 1 2 3
    r sadd set:listpack a b c
    # Over set-max-listpack-entries.
    set members {}
    for {set i 0} {$i < 200} {incr i} {
        lappend members m$i
    }
    r sadd set:hashtable {*}$members
    # Members with the same score are ordered by name.
    r zadd zset:listpack 1 a 1 b 2 c
    # Over zset-max-listpack-value.
    r zadd zset:btree 1 [string repeat z 100] 1 b 2 c
    r hset hash:listpack f1 v1 f2 v2
    # Over hash-max-listpack-value.
    r hset hash:hashtable f1 [string repeat h 100] f2 v2
    # Two consumer groups, one with a pending entry.
    r xadd stream 1-0 f v1
    r xadd stream 2-0 f v2
    r xgroup create stream g1 0
    r xgroup create stream g2 $
    r xreadgroup group g1 c1 count 1 streams stream >
    set encodings {
        string:binary raw string:large raw
        list:listpack listpack list:quicklist quicklist
        set:intset intset set:listpack listpack set:hashtable hashtable
        zset:listpack listpack zset:btree btree
        hash:listpack listpack hash:hashtable hashtable
        stream stream
    }
    dict for {key encoding} $encodings {
        assert_encoding $encoding $key
    }
    return $encodings
}

# For each key build_every_type creates, a read and a write. KEY stands for the
# key.
set reads_and_writes {
    string:binary   {getrange KEY 0 300}            {append KEY y}
    string:large    {strlen KEY}                    {setrange KEY 0 y}
    list:listpack   {lindex KEY 1}                  {lpush KEY new}
    list:quicklist  {lrange KEY 0 -1}               {rpop KEY}
    set:intset      {sismember KEY 2}               {sadd KEY 4}
    set:listpack    {smembers KEY}                  {srem KEY a}
    set:hashtable   {scard KEY}                     {sadd KEY new}
    zset:listpack   {zrange KEY 0 -1 withscores}    {zadd KEY 3 d}
    zset:btree      {zscore KEY b}                  {zincrby KEY 5 b}
    hash:listpack   {hgetall KEY}                   {hset KEY f3 v3}
    hash:hashtable  {hget KEY f1}                   {hdel KEY f2}
    stream          {xinfo stream KEY full}         {xadd KEY 3-0 f v3}
}

# Spill and fetch of every type, and clients waiting on a fetch.
if {$tiering_built} {
  foreach engine {mock flashcache} {
    # The valkey server runs in its own directory, so the path is absolute.
    set path [file normalize [tmpdir ext-storage-fetch-$engine]]
    start_server [list tags {"ext-storage external:skip"} overrides [list ext-storage-enabled yes ext-storage-engine $engine ext-storage-path $path ext-storage-capacity 128mb save ""]] {
        test "Every type and encoding comes back from flash unchanged ($engine)" {
            r flushall
            set encodings [build_every_type]
            set keys [dict keys $encodings]
            set digests [r debug digest-value {*}$keys]
            # The stream digest leaves out consumer groups.
            set stream_info [r xinfo stream stream full]
            # Each later round spills values that were fetched in the round before.
            for {set round 0} {$round < 3} {incr round} {
                set spilled [ext_storage_info total_num_items_spilled_to_storage]
                set fetched [fetched_keys]
                spill_and_wait {*}$keys
                assert_equal [expr {$spilled + [llength $keys]}] [ext_storage_info total_num_items_spilled_to_storage]
                # OBJECT ENCODING names the key, so it fetches the value.
                dict for {key encoding} $encodings {
                    assert_equal $encoding [r object encoding $key]
                    assert_match {* storage_state:0 *} [r debug object $key fast]
                }
                assert_equal [expr {$fetched + [llength $keys]}] [fetched_keys]
                assert_equal $digests [r debug digest-value {*}$keys]
                assert_equal $stream_info [r xinfo stream stream full]
            }
        } {} {needs:debug}

        test "Keys with the same name in two databases come back with their own values ($engine)" {
            r flushall
            set val9 [string repeat a 1000]
            set val10 [string repeat b 1000]
            r set k $val9
            r select 10
            r set k $val10
            spill_and_wait k
            r select 9
            spill_and_wait k
            set fetched [fetched_keys]
            assert_equal $val9 [r get k]
            r select 10
            assert_equal $val10 [r get k]
            assert_equal [expr {$fetched + 2}] [fetched_keys]

            # A spill in one database leaves the key in the other in memory.
            spill_and_wait k
            r select 9
            assert_match {* storage_state:0 *} [r debug object k fast]
            assert_equal $val9 [r get k]
            r select 10
            set got10 [r get k]
            r select 9
            assert_equal $val10 $got10
        } {} {needs:debug}

        test "Reading a key on flash gives the same reply as reading it in memory ($engine)" {
            r flushall
            build_every_type
            foreach {key read write} $reads_and_writes {
                r copy $key mem:$key
                spill_and_wait $key
                set fetched [fetched_keys]
                set flash_reply [r {*}[string map [list KEY $key] $read]]
                set mem_reply [r {*}[string map [list KEY mem:$key] $read]]
                # Sets and hashes are unordered.
                if {[string match {set:*} $key] || [string match {hash:*} $key]} {
                    set flash_reply [lsort $flash_reply]
                    set mem_reply [lsort $mem_reply]
                }
                assert_equal $mem_reply $flash_reply
                assert_equal [expr {$fetched + 1}] [fetched_keys]
                assert_match {* storage_state:0 *} [r debug object $key fast]
            }

            # One command that names a key in memory and a key on flash waits
            # for the fetch and replies with both values.
            set val [string repeat x 1000]
            r set mem v
            r set flash $val
            spill_and_wait flash
            set fetched [fetched_keys]
            assert_equal [list v $val] [r mget mem flash]
            assert_equal [expr {$fetched + 1}] [fetched_keys]
            assert_match {* storage_state:0 *} [r debug object flash fast]

            # DEL also waits for the fetch before it deletes the key.
            spill_and_wait flash
            assert_equal 1 [r del flash]
            assert_equal 0 [r exists flash]
        } {} {needs:debug}

        test "Writing a key on flash gives the same result as writing it in memory ($engine)" {
            r flushall
            build_every_type
            foreach {key read write} $reads_and_writes {
                r copy $key mem:$key
                spill_and_wait $key
                set fetched [fetched_keys]
                set mem_reply [r {*}[string map [list KEY mem:$key] $write]]
                assert_equal $mem_reply [r {*}[string map [list KEY $key] $write]]
                assert_equal [expr {$fetched + 1}] [fetched_keys]
                assert_equal [r debug digest-value mem:$key] [r debug digest-value $key]
                # The changed value spills and comes back too. TYPE names the
                # key, so it fetches the value.
                spill_and_wait $key
                assert_equal [r type mem:$key] [r type $key]
                assert_equal [r debug digest-value mem:$key] [r debug digest-value $key]
            }
        } {} {needs:debug}

        test "Two clients waiting on one key on flash share its fetch and run in order ($engine)" {
            r flushall
            set val [string repeat x 1000]
            r set k $val
            spill_and_wait k
            set fetched [fetched_keys]
            # Completions aren't handled while the foreground is paused, so both
            # clients stay blocked on the fetch.
            r debug ext-storage pause-foreground 1
            set reader [valkey_deferring_client]
            set writer [valkey_deferring_client]
            $reader get k
            wait_for_blocked 1
            $writer append k y
            wait_for_blocked 2
            assert_match {* storage_state:3 *} [r debug object k fast]
            assert_equal 1 [ext_storage_info num_items_fetching_from_storage]
            r debug ext-storage pause-foreground 0
            assert_equal $val [$reader read]
            assert_equal 1001 [$writer read]
            $reader close
            $writer close
            assert_equal ${val}y [r get k]
            assert_equal [expr {$fetched + 1}] [fetched_keys]
        } {} {needs:debug}

        # Clients wait on a key by name in every database, so a fetch in one
        # database can wake a client waiting on the other, which then waits
        # again. Lifting the pause hands over both completions at once, so this
        # checks the replies but may not reach that case.
        test "Clients waiting on keys with the same name in two databases get their own values ($engine)" {
            r flushall
            set val9 [string repeat a 1000]
            set val10 [string repeat b 1000]
            r set k $val9
            spill_and_wait k
            r select 10
            r set k $val10
            spill_and_wait k
            r select 9
            set fetched [fetched_keys]
            r debug ext-storage pause-foreground 1
            set rd9 [valkey_deferring_client]
            set rd10 [valkey_deferring_client]
            $rd10 select 10
            assert_equal OK [$rd10 read]
            $rd9 get k
            $rd10 get k
            wait_for_blocked 2
            assert_equal 2 [ext_storage_info num_items_fetching_from_storage]
            r debug ext-storage pause-foreground 0
            assert_equal $val9 [$rd9 read]
            assert_equal $val10 [$rd10 read]
            $rd9 close
            $rd10 close
            assert_equal [expr {$fetched + 2}] [fetched_keys]
        } {} {needs:debug}
    }
    file delete -force $path
  }
}

# A valkey server built without data tiering refuses to start with it enabled.
if {$tiering_built} {
  foreach engine {mock flashcache} {
    # The valkey server runs in its own directory, so the path is absolute.
    set path [file normalize [tmpdir ext-storage-$engine]]
    start_server [list tags {"ext-storage external:skip"} overrides [list ext-storage-enabled yes ext-storage-engine $engine ext-storage-path $path ext-storage-capacity 128mb save ""]] {
        test "The throttle is on by default ($engine)" {
            assert_equal yes [lindex [r config get ext-storage-throttle-enabled] 1]
        }

        if {$engine eq "flashcache"} {
            test "INFO storage_engine reports every FlashCache metric" {
                set info [r info storage_engine]
                foreach field {
                    total_num_items allocated_db_size_bytes total_num_requests_throttled
                    total_num_items_evicted total_num_disk_reads total_num_disk_writes
                    db_size_bytes active_db_size_bytes free_allocatable_db_size_bytes
                    db_used_over_allocatable_bytes pending_flush_bytes fc_used_memory_bytes
                    total_disk_read_bytes total_disk_write_bytes num_reads_in_flight
                    total_num_read_requests total_num_write_requests total_num_delete_requests
                    total_num_retryable_disk_errors total_num_unused_disk_reads
                    total_unused_disk_read_bytes total_num_partial_item_reads
                    total_partial_item_read_bytes total_num_optimized_deletes index_growing
                    total_num_index_growths total_evicted_bytes evicting_under_max_log_size
                    total_num_items_evicted_under_max_log_size gc_rate_bytes_per_sec
                    min_gc_rate_bytes_per_sec total_gc_num_disk_reads total_gc_read_bytes
                    total_gc_write_bytes total_gc_num_items_moved num_requests_queued
                } {
                    assert_match "*storage_engine_$field:*" $info
                }
                # Each latency histogram is one line with nine buckets.
                foreach histogram {disk_read_latency_usec disk_write_latency_usec} {
                    assert_match "*storage_engine_$histogram:0-100=*,100000-inf=*" $info
                    set line [lsearch -inline [split $info "\r\n"] "storage_engine_$histogram:*"]
                    assert_equal 9 [llength [split [lindex [split $line :] 1] ,]]
                }
            }
        }

        test "DEBUG EXT-STORAGE SPILL submits a key and refuses keys it can't spill ($engine)" {
            r set big [string repeat x 1000]
            r set small v
            assert_equal OK [r debug ext-storage spill big]
            wait_for_flash big
            # A key already on flash stays there.
            assert_equal OK [r debug ext-storage spill big]
            assert_match {* storage_state:2 *} [r debug object big fast]
            # A short value is embedded in the key, so it can't be spilled.
            assert_error {*key 'small' can't be spilled*} {r debug ext-storage spill small}
            # No key, or a subcommand that doesn't exist.
            assert_error {*unknown subcommand*} {r debug ext-storage spill}
            assert_error {*unknown subcommand*} {r debug ext-storage nosuch}
        } {} {needs:debug}

        test "FLUSHALL with a held spill lifts the pauses and finishes ($engine)" {
            r flushall
            r set held [string repeat x 1000]
            r debug ext-storage pause-foreground 1
            r debug ext-storage pause-background 1
            assert_equal OK [r debug ext-storage spill held]
            assert_equal OK [r flushall]
            assert_equal 0 [r dbsize]
            assert_equal 0 [getInfoProperty [r info ext_storage] ext_storage_foreground_paused]
            assert_equal 0 [getInfoProperty [r info ext_storage] ext_storage_background_paused]
            verify_log_message 0 "*Lifting the DEBUG pause*to drain spills in flight*" 0
        } {} {needs:debug}

        test "A fetch that finds the queue full is deferred and the valkey server keeps serving ($engine)" {
            r flushall
            r set onflash [string repeat x 1000]
            assert_equal OK [r debug ext-storage spill onflash]
            wait_for_flash onflash
            fill_storage_queue

            # A fetch now has no slot. The client waits, the main thread does not.
            set rd [start_deferred_get onflash]
            assert_equal PONG [r ping]
            assert_equal 1 [getInfoProperty [r info] blocked_clients]

            # Once the storage engine runs again, the fetch goes in and the client gets its value.
            r debug ext-storage pause-background 0
            assert_equal [string repeat x 1000] [$rd read]
            $rd close
            wait_for_condition 50 100 {
                [getInfoProperty [r info ext_storage] ext_storage_num_fetches_deferred] == 0
            } else {
                fail "the deferred fetch was not retried"
            }
            assert {[getInfoProperty [r info ext_storage] ext_storage_total_num_fetches_deferred] >= 1}
        } {} {needs:debug}

        test "FLUSHALL drops a deferred fetch and the client sees the key gone ($engine)" {
            r flushall
            r set onflash [string repeat x 1000]
            assert_equal OK [r debug ext-storage spill onflash]
            wait_for_flash onflash
            fill_storage_queue
            set rd [start_deferred_get onflash]
            set fetched [getInfoProperty [r info ext_storage] ext_storage_total_num_items_fetched_from_storage]

            # The flush lifts the pause and empties the keyspace. The deferred fetch
            # finds its key gone, so it is dropped and the GET runs again on nothing.
            assert_equal OK [r flushall]
            assert_equal {} [$rd read]
            $rd close
            wait_for_condition 50 100 {
                [getInfoProperty [r info ext_storage] ext_storage_num_fetches_deferred] == 0
            } else {
                fail "the deferred fetch was not dropped"
            }
            assert_equal $fetched [getInfoProperty [r info ext_storage] ext_storage_total_num_items_fetched_from_storage]
            assert_equal 0 [getInfoProperty [r info] blocked_clients]
        } {} {needs:debug}

        test "DEBUG EXT-STORAGE SPILL submits several keys and stops at the first one that fails ($engine)" {
            r flushall
            r set a [string repeat x 1000]
            r set b [string repeat x 1000]
            r set c [string repeat x 1000]
            assert_equal OK [r debug ext-storage spill a b]
            wait_for_flash a b
            # c is submitted before the missing key stops the command.
            assert_error {*No such key 'missing'*} {r debug ext-storage spill c missing}
            wait_for_flash c
        } {} {needs:debug}

        test "DEBUG EXT-STORAGE SPILL refuses more than 256 keys and submits none ($engine)" {
            r flushall
            set keys {}
            for {set i 0} {$i < 257} {incr i} {
                r set key:$i [string repeat x 1000]
                lappend keys key:$i
            }
            assert_error {*at most 256 keys*} {r debug ext-storage spill {*}$keys}
            assert_match {* storage_state:0 *} [r debug object key:0 fast]
            assert_equal OK [r debug ext-storage spill {*}[lrange $keys 0 255]]
            wait_for_condition 100 50 {
                [getInfoProperty [r info ext_storage] ext_storage_num_items_spilling_to_storage] == 0
            } else {
                puts [r info ext_storage]
                puts [exec tail -n 20 < [srv 0 stdout]]
                fail "spills didn't complete"
            }
            assert_match {* storage_state:2 *} [r debug object key:255 fast]
        } {} {needs:debug}

        # The mock storage engine finishes requests when they are polled, so only
        # FlashCache can leave a completion waiting while the event loop sleeps.
        if {$engine eq "flashcache"} {
            test "Spills and fetches on an idle server don't wait for serverCron, and the keep-awake timer stops" {
                r flushall
                set keys {}
                for {set i 0} {$i < 5} {incr i} {
                    r set key:$i [string repeat x 1000]
                    lappend keys key:$i
                }
                # At hz 1, a request that waits for serverCron takes about half a second.
                set hz [lindex [r config get hz] 1]
                r config set hz 1
                set wakeups [getInfoProperty [r info ext_storage] ext_storage_total_num_keep_awake_wakeups]
                assert_equal OK [r debug ext-storage spill {*}$keys]
                # Read once, since each command wakes the event loop and collects what finished.
                after 100
                set spilling [getInfoProperty [r info ext_storage] ext_storage_num_items_spilling_to_storage]
                set start [clock milliseconds]
                foreach key $keys {
                    assert_equal [string repeat x 1000] [r get $key]
                }
                set elapsed [expr {[clock milliseconds] - $start}]
                # With nothing in flight, a running timer would add about 500 cycles.
                set cycles [s eventloop_cycles]
                after 500
                set idle_cycles [expr {[s eventloop_cycles] - $cycles}]
                r config set hz $hz
                assert_equal 0 $spilling
                assert_lessthan $elapsed 500
                assert_morethan [getInfoProperty [r info ext_storage] ext_storage_total_num_keep_awake_wakeups] $wakeups
                assert_lessthan $idle_cycles 50
            } {} {needs:debug}
        }

        test "DEBUG commands that read a value fail on a key on flash ($engine)" {
            r flushall
            set str [string repeat x 1000]
            set elem [string repeat y 10000]
            set field [string repeat z 1000]
            r set str $str
            # Over list-max-listpack-size, so it's a quicklist.
            r rpush list $elem
            assert_encoding quicklist list
            # Over hash-max-listpack-value, so it's a hashtable.
            r hset hash f $field
            assert_encoding hashtable hash
            r set mem v
            set mem_digest [lindex [r debug digest-value mem] 0]
            assert_equal OK [r debug ext-storage spill str list hash]
            wait_for_flash str list hash

            assert_error {*value of 'str' isn't in memory*} {r debug sdslen str}
            assert_error {*value of 'str' isn't in memory*} {r debug listpack str}
            assert_error {*value of 'list' isn't in memory*} {r debug quicklist list}
            assert_error {*value of 'hash' isn't in memory*} {r debug htstats-key hash}
            assert_error {*value of 'list' isn't in memory*} {r debug digest-value mem list}
            assert_error {*value isn't in memory*} {r debug digest}
            # A key in memory still works.
            assert_equal $mem_digest [lindex [r debug digest-value mem] 0]
            assert_match {*val_sds_len:1*} [r debug sdslen mem]
            # The refused commands left the values intact.
            assert_equal $str [r get str]
            assert_equal [list $elem] [r lrange list 0 -1]
            assert_equal $field [r hget hash f]
        } {} {needs:debug}

        test "DEBUG OBJECT reports a key on flash without reading its value ($engine)" {
            r flushall
            # Over list-max-listpack-size, so it's a quicklist.
            r rpush list [string repeat x 10000]
            assert_encoding quicklist list
            assert_equal OK [r debug ext-storage spill list]
            wait_for_flash list
            set info [r debug object list]
            assert_match {* storage_state:2 *} $info
            assert_no_match {*serializedlength*} $info
            assert_no_match {*ql_nodes*} $info
        } {} {needs:debug}

        # Active defrag needs the jemalloc the source distribution ships, as in memefficiency.tcl.
        if {[string match {*jemalloc*} [s mem_allocator]] && [r debug mallctl arenas.page] <= 8192} {
            test "Active defrag skips keys on flash ($engine)" {
                r flushall
                for {set i 0} {$i < 100} {incr i} {
                    r set key:$i [string repeat x 1000]
                    r rpush list:$i [string repeat x 1000]
                    assert_equal OK [r debug ext-storage spill key:$i]
                    assert_equal OK [r debug ext-storage spill list:$i]
                    # Keys in memory, so the scan visibly makes progress.
                    r set mem:$i [string repeat x 1000]
                }
                for {set i 0} {$i < 100} {incr i} {
                    wait_for_flash key:$i list:$i
                }
                set visited [expr {[s active_defrag_key_hits] + [s active_defrag_key_misses]}]
                r config set active-defrag-ignore-bytes 1
                r config set active-defrag-threshold-lower 0
                r config set activedefrag yes
                # Wait for a full pass, which visits every key in memory. Keys on
                # flash are skipped and not counted.
                wait_for_condition 100 50 {
                    [s active_defrag_key_hits] + [s active_defrag_key_misses] - $visited >= 100
                } else {
                    r config set activedefrag no
                    puts [r info ext_storage]
                    puts [exec tail -n 20 < [srv 0 stdout]]
                    fail "active defrag didn't run"
                }
                r config set activedefrag no
                assert_equal PONG [r ping]
                assert_match {* storage_state:2 *} [r debug object list:0 fast]
                # Defrag left the values intact, on flash and in memory.
                assert_equal [string repeat x 1000] [r get key:0]
                assert_equal [string repeat x 1000] [r get mem:0]
            } {} {needs:debug}
        }
    }
    file delete -force $path
  }
}

proc spilled_keys {{level 0}} {
    return [getInfoProperty [r $level info ext_storage] ext_storage_total_num_items_spilled_to_storage]
}

# The memory spilling compares with maxmemory: used memory without replica and
# AOF buffers. A full INFO counts its own partly built reply, so this reads
# INFO memory, whose section comes first.
proc counted_memory {{level 0}} {
    set info [r $level info memory]
    return [expr {[getInfoProperty $info used_memory] - [getInfoProperty $info mem_not_counted_for_evict]}]
}

# Waits until no spill is in flight on valkey server 'level'.
proc wait_for_no_spills {{level 0}} {
    wait_for_condition 100 50 {
        [getInfoProperty [r $level info ext_storage] ext_storage_num_items_spilling_to_storage] == 0
    } else {
        fail_with_state "spills are still in flight" $level
    }
}

proc throttle_info {field} {
    return [getInfoProperty [r info throttling] ext_storage_throttle_$field]
}

# Puts 300 keys on flash and leaves 100 in memory, with nothing left to spill.
proc mostly_on_flash {} {
    r flushall
    r config set maxmemory 0
    set val [string repeat x 1000]
    for {set i 0} {$i < 300} {incr i} {
        r set flash:$i $val
    }
    # A hash with a field TTL can't be spilled.
    for {set i 0} {$i < 100} {incr i} {
        r hset mem:$i f $val
        r hexpire mem:$i 100000 FIELDS 1 f
    }
    # Far below what is in use, so every spillable key spills, and
    # noeviction keeps eviction from taking any. Sampling every key
    # keeps the last few from waiting on unlucky samples.
    set before [spilled_keys]
    r config set maxmemory-samples 64
    r config set maxmemory-policy noeviction
    r config set maxmemory 1
    wait_for_condition 100 100 {
        [spilled_keys] - $before >= 300 &&
        [getInfoProperty [r info ext_storage] ext_storage_num_items_spilling_to_storage] == 0
    } else {
        fail_with_state "the strings weren't all spilled"
    }
    r config set maxmemory 0
    r config set maxmemory-samples 5
    r config resetstat
}

# A server built without data tiering refuses to start with it enabled.
if {$tiering_built} {
  foreach engine {mock flashcache} {
    # The valkey server runs in its own directory, so the paths are absolute.
    # The throttle starts off here, since its tests change one percent at a time.
    set path [file normalize [tmpdir ext-storage-$engine]]
    start_server [list tags {"ext-storage external:skip"} overrides [list ext-storage-enabled yes ext-storage-engine $engine ext-storage-path $path ext-storage-capacity 128mb ext-storage-throttle-enabled no save ""]] {
        test "Keys over maxmemory are spilled instead of evicted ($engine)" {
            r config set maxmemory-policy allkeys-lru
            set val [string repeat x 1000]
            for {set i 0} {$i < 2000} {incr i} {
                r set key:$i $val
            }
            r config set maxmemory [expr {[s used_memory] * 19 / 20}]
            for {set i 0} {$i < 200} {incr i} {
                r set new:$i $val
            }
            wait_for_condition 50 100 {
                [counted_memory] <= [s maxmemory]
            } else {
                fail_with_state "used_memory didn't drop below maxmemory"
            }
            assert_morethan [spilled_keys] 0
            assert_equal 0 [s evicted_keys]
            r config set maxmemory 0
            # A batch of keys waits for its fetches once. TODO(#136): over 256
            # fetches at once fill the storage engine's queue.
            set keys {}
            for {set i 0} {$i < 2000} {incr i} {
                lappend keys key:$i
            }
            for {set i 0} {$i < 200} {incr i} {
                lappend keys new:$i
            }
            for {set i 0} {$i < [llength $keys]} {incr i 200} {
                foreach value [r mget {*}[lrange $keys $i [expr {$i + 199}]]] {
                    assert_equal $val $value
                }
            }
        }

        # Eviction skips keys on flash, so it gives up rather than resample
        # forever once nothing in memory can be evicted.
        test "Writes fail with OOM once nothing can be spilled or evicted ($engine)" {
            r config set maxmemory 1
            set err {}
            for {set i 0} {$i < 1000} {incr i} {
                if {[catch {r set small:$i v} err]} break
            }
            assert_match {OOM*} $err
            r config set maxmemory 0
        }

        test "Spilled hashes and lists can be deleted and overwritten ($engine)" {
            r flushall
            r config set maxmemory 0
            set val [string repeat x 100]
            for {set i 0} {$i < 100} {incr i} {
                r hset small_hash:$i f $val
                r rpush list:$i $val $val
                # Over hash-max-listpack-entries, so it's a hashtable.
                for {set f 0} {$f < 130} {incr f} {
                    lappend fields f$f $val
                }
                r hset big_hash:$i {*}$fields
                unset fields
            }
            r config set maxmemory [expr {[s used_memory] * 19 / 20}]
            r set trigger $val
            wait_for_condition 50 100 {
                [counted_memory] <= [s maxmemory]
            } else {
                fail_with_state "used_memory didn't drop below maxmemory"
            }
            r config set maxmemory 0
            set on_flash 0
            for {set i 0} {$i < 100} {incr i} {
                foreach key [list small_hash:$i big_hash:$i list:$i] {
                    if {![string match {* storage_state:0 *} [r debug object $key fast]]} {
                        incr on_flash
                    }
                }
            }
            assert_morethan $on_flash 0
            for {set i 0} {$i < 100} {incr i} {
                r del small_hash:$i big_hash:$i
                r set list:$i v
            }
            assert_equal 101 [r dbsize]
            assert_equal 0 [r exists small_hash:0 big_hash:0]
            assert_equal v [r get list:0]
        } {} {needs:debug}

        test "Keys over maxmemory are spilled under noeviction ($engine)" {
            r flushall
            r config set maxmemory 0
            r config set maxmemory-policy noeviction
            r config resetstat
            set val [string repeat x 1000]
            for {set i 0} {$i < 1000} {incr i} {
                r set key:$i $val
            }
            set before [spilled_keys]
            r config set maxmemory [expr {[s used_memory] * 19 / 20}]
            catch {r set trigger v}
            wait_for_condition 50 100 {
                [spilled_keys] > $before
            } else {
                fail_with_state "no key was spilled"
            }
            assert_equal 0 [s evicted_keys]
            r config set maxmemory 0
            r config set maxmemory-policy allkeys-lru
        }

        # Once the mean is known, a small deficit spills a few keys, not 64.
        test "Spills only what the deficit needs ($engine)" {
            r flushall
            r config set maxmemory 0
            set val [string repeat x 1000]
            for {set i 0} {$i < 2000} {incr i} {
                r set key:$i $val
            }
            # The first spills size the mean.
            r config set maxmemory [expr {[s used_memory] - 50000}]
            r set trigger v
            wait_for_no_spills
            wait_for_condition 50 100 {
                [counted_memory] <= [s maxmemory]
            } else {
                fail_with_state "used_memory didn't drop below maxmemory"
            }
            # About 10 values cover a 10 KB deficit. The bound leaves room for
            # values freed in the background.
            set before [spilled_keys]
            r config set maxmemory [expr {[s used_memory] - 10000}]
            r set trigger v
            wait_for_no_spills
            set spilled [expr {[spilled_keys] - $before}]
            assert_morethan $spilled 0
            assert_lessthan_equal $spilled 30
            r config set maxmemory 0
        }

        # One sample can land only on keys already on flash. Selection samples
        # again, so writes go on while spillable keys remain.
        test "Writes don't fail with OOM while most keys are on flash ($engine)" {
            r flushall
            r config set maxmemory 0
            set val [string repeat x 1000]
            set keys {}
            for {set i 0} {$i < 2000} {incr i} {
                r set key:$i $val
                if {$i < 1500} { lappend keys key:$i }
            }
            for {set i 0} {$i < 1500} {incr i 200} {
                r debug ext-storage spill {*}[lrange $keys $i [expr {$i + 199}]]
                wait_for_no_spills
            }
            r config set maxmemory [expr {[s used_memory] - 20000}]
            for {set i 0} {$i < 300} {incr i} {
                r set new:$i $val
            }
            r config set maxmemory 0
            assert_equal 0 [s evicted_keys]
        } {} {needs:debug}

        # TODO(#143): test that a sustained write load keeps spilling without
        # OOM once writes are throttled. Without throttling, writes can outpace
        # spills under load and reach the OOM cap.

        # The mean of 200 byte and 50 KB values fits neither, but used memory
        # still comes back under maxmemory and no write is refused.
        test "Values of mixed sizes are spilled under maxmemory without OOM ($engine)" {
            r flushall
            r config set maxmemory 0
            set small [string repeat x 200]
            set big [string repeat x 50000]
            for {set i 0} {$i < 500} {incr i} {
                r set key:$i [expr {$i % 10 == 0 ? $big : $small}]
            }
            r config set maxmemory [expr {[s used_memory] * 9 / 10}]
            for {set i 0} {$i < 300} {incr i} {
                r set new:$i [expr {$i % 10 == 0 ? $big : $small}]
            }
            wait_for_no_spills
            wait_for_condition 50 100 {
                [counted_memory] <= [s maxmemory]
            } else {
                fail_with_state "used_memory didn't drop below maxmemory"
            }
            assert_equal 0 [s evicted_keys]
            r config set maxmemory 0
        }

        test "Keys on flash can be flushed ($engine)" {
            mostly_on_flash
            r flushall
            assert_equal 0 [r dbsize]
            r set key v
            assert_equal v [r get key]
            # Saving a key on flash crashes, so this also shows none is left.
            assert_equal OK [r save]
        } {} {needs:save}

        test "ext-storage-spill-policy must agree with an LRU or LFU maxmemory-policy ($engine)" {
            r config set maxmemory-policy allkeys-lru
            assert_error {*must match*} {r config set ext-storage-spill-policy allkeys-lfu}
            assert_error {*must match*} {r config set maxmemory-policy allkeys-lfu}
            r config set maxmemory-policy allkeys-lfu ext-storage-spill-policy allkeys-lfu
            r config set maxmemory-policy noeviction
            r config set ext-storage-spill-policy allkeys-lru
            r config set maxmemory-policy allkeys-lru
        }

        test "OBJECT FREQ and RDB follow an LFU spill policy under noeviction ($engine)" {
            r flushall
            r config set maxmemory-policy noeviction ext-storage-spill-policy allkeys-lfu
            r config set lfu-log-factor 0
            r set key v
            for {set i 0} {$i < 20} {incr i} { r get key }
            set freq [r object freq key]
            assert_morethan $freq 20
            assert_error {*idle time not tracked*} {r object idletime key}
            r debug reload
            assert_equal $freq [r object freq key]
            r config set lfu-log-factor 10
            r config set maxmemory-policy allkeys-lru ext-storage-spill-policy allkeys-lru
        } {OK} {needs:debug}

        foreach policy {allkeys-random allkeys-lru allkeys-lfu} {
            test "Writes fail with OOM instead of evicting under $policy ($engine)" {
                mostly_on_flash
                # An LFU maxmemory-policy needs spilling to rank by LFU too.
                set spill_policy [expr {$policy eq "allkeys-lfu" ? "allkeys-lfu" : "allkeys-lru"}]
                r config set maxmemory-policy $policy ext-storage-spill-policy $spill_policy
                # Nothing is left to spill, and memory is 130% of maxmemory, so the write is refused.
                r config set maxmemory [expr {[s used_memory] * 100 / 130}]
                assert_error {OOM*} {r set trigger v}
                r config set maxmemory 0
                r config set maxmemory-policy allkeys-lru ext-storage-spill-policy allkeys-lru
                assert_equal 0 [s evicted_keys]
                assert_equal 400 [r dbsize]
                assert_equal [string repeat x 1000] [r get flash:0]
            }
        }

        test "Writes succeed below 120% of maxmemory while nothing is left to spill ($engine)" {
            mostly_on_flash
            r config set maxmemory-policy allkeys-lru ext-storage-spill-policy allkeys-lru
            set misses [getInfoProperty [r info ext_storage] ext_storage_total_num_spill_candidate_misses]
            set keys [r dbsize]
            r config set maxmemory [expr {[s used_memory] * 100 / 110}]
            assert_equal OK [r set trigger v]
            assert_morethan [getInfoProperty [r info ext_storage] ext_storage_total_num_spill_candidate_misses] $misses
            r config set maxmemory 0
            assert_equal 0 [s evicted_keys]
            assert_equal [expr {$keys + 1}] [r dbsize]
        }

        test "The throttle slows commands in proportion to used memory past maxmemory ($engine)" {
            r flushall
            # A hash with a field TTL can't be spilled, so used memory stays put.
            set val [string repeat x 1000]
            for {set i 0} {$i < 2000} {incr i} {
                r hset mem:$i f $val
                r hexpire mem:$i 100000 FIELDS 1 f
            }
            r set flashed $val
            r set deleted $val
            r debug ext-storage spill flashed deleted
            wait_for_flash flashed deleted
            set used [s used_memory]
            # The percents must keep measure <= start < max while the throttle is on.
            r config set ext-storage-throttle-max-percent 104
            assert_error {*must keep*} {r config set ext-storage-throttle-enabled yes}
            r config set ext-storage-throttle-max-percent 120
            r config set ext-storage-throttle-enabled yes
            assert_error {*must keep*} {r config set ext-storage-throttle-measure-percent 110}
            # By default throttling starts at 105% and is full at 120%.
            foreach {percent min max} {112 0.3 0.7 125 1 1 50 0 0} {
                r config set maxmemory [expr {$used * 100 / $percent}]
                wait_for_condition 50 100 {
                    [throttle_info fraction] >= $min && [throttle_info fraction] <= $max
                } else {
                    fail_with_state "the throttle fraction at $percent% wasn't between $min and $max"
                }
            }

            # Past the max percent a read of a value on flash is held until
            # memory falls back. A read of a value in memory isn't.
            r config set maxmemory [expr {$used * 100 / 125}]
            wait_for_condition 50 100 {
                [throttle_info fraction] == 1
            } else {
                fail_with_state "the throttle didn't reach a full stop"
            }
            assert_equal $val [r hget mem:0 f]
            # Deleting a value on flash frees memory, so it isn't held.
            set rd [valkey_deferring_client]
            $rd del deleted
            after 300
            assert_equal 0 [throttle_info queued_clients]
            assert_equal 1 [$rd read]
            $rd get flashed
            wait_for_condition 50 100 {
                [throttle_info queued_clients] == 1
            } else {
                fail_with_state "the read wasn't held"
            }
            r config set maxmemory [expr {$used * 2}]
            assert_equal $val [$rd read]

            # Disabling the throttle releases held clients.
            r debug ext-storage spill flashed
            wait_for_flash flashed
            r config set maxmemory [expr {$used * 100 / 125}]
            wait_for_condition 50 100 {
                [throttle_info fraction] == 1
            } else {
                fail_with_state "the throttle didn't reach a full stop again"
            }
            $rd get flashed
            wait_for_condition 50 100 {
                [throttle_info queued_clients] == 1
            } else {
                fail_with_state "the read wasn't held again"
            }
            r config set ext-storage-throttle-enabled no
            assert_equal $val [$rd read]
            $rd close
            r config set maxmemory 0
            r flushall
        } {OK} {needs:debug}

        test "A write is held at a full stop and runs once memory falls back ($engine)" {
            r flushall
            set val [string repeat x 1000]
            for {set i 0} {$i < 2000} {incr i} {
                r hset mem:$i f $val
                r hexpire mem:$i 100000 FIELDS 1 f
            }
            # Nothing else can be spilled, and this spill never completes, so memory stays put.
            r set inflight $val
            r debug ext-storage pause-background 1
            r debug ext-storage pause-foreground 1
            r debug ext-storage spill inflight
            set used [s used_memory]
            r config set ext-storage-throttle-max-percent 110 ext-storage-throttle-enabled yes
            r config set maxmemory [expr {$used * 100 / 112}]
            wait_for_condition 50 100 {
                [throttle_info fraction] == 1
            } else {
                fail_with_state "the throttle didn't reach a full stop"
            }
            set rd [valkey_deferring_client]
            $rd set written v
            wait_for_condition 50 100 {
                [throttle_info queued_clients] == 1
            } else {
                fail_with_state "the write wasn't held"
            }
            assert_equal 0 [r exists written]
            r config set maxmemory [expr {$used * 2}]
            assert_equal OK [$rd read]
            assert_equal v [r get written]
            $rd close
            r debug ext-storage pause-foreground 0
            r debug ext-storage pause-background 0
            wait_for_no_spills
            r config set ext-storage-throttle-enabled no ext-storage-throttle-max-percent 120 maxmemory 0
            r flushall
        } {OK} {needs:debug}
    }
    file delete -force $path

    # Each replica decides what to spill from its own reads.
    set primary_path [file normalize [tmpdir ext-storage-primary-$engine]]
    set replica_path [file normalize [tmpdir ext-storage-replica-$engine]]
    start_server [list tags {"ext-storage repl external:skip"} overrides [list ext-storage-enabled yes ext-storage-engine $engine ext-storage-path $primary_path ext-storage-capacity 128mb save ""]] {
    start_server [list overrides [list ext-storage-enabled yes ext-storage-engine $engine ext-storage-path $replica_path ext-storage-capacity 128mb save ""]] {
        set primary [srv -1 client]
        set replica [srv 0 client]

        test "A replica spills on its own and never evicts ($engine)" {
            set val [string repeat x 1000]
            for {set i 0} {$i < 1000} {incr i} {
                $primary set key:$i $val
            }
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up} && [$replica dbsize] == 1000
            } else {
                fail_with_state "replica didn't sync"
            }
            $replica config set maxmemory-policy allkeys-lru
            $replica config set maxmemory [expr {[s 0 used_memory] - 200000}]
            $replica ping
            wait_for_condition 50 100 {
                [spilled_keys] > 0
            } else {
                fail_with_state "replica didn't spill"
            }
            assert_equal 0 [s 0 evicted_keys]
            assert_equal 1000 [$replica dbsize]
            assert_equal 0 [spilled_keys -1]
            $replica config set maxmemory 0
            assert_equal $val [$replica get key:0]
            assert_equal $val [$replica get key:999]
        }

        test "A replica applies its primary's writes while its throttle allows nothing ($engine)" {
            $primary flushall
            set val [string repeat x 1000]
            # Keys with a TTL can't be spilled, so the replica's memory stays put.
            for {set i 0} {$i < 1000} {incr i} {
                $primary set ttl:$i $val EX 100000
            }
            wait_for_condition 50 100 {
                [$replica dbsize] == 1000
            } else {
                fail_with_state "replica didn't sync"
            }
            $replica config set ext-storage-throttle-start-percent 100 ext-storage-throttle-max-percent 101 ext-storage-throttle-enabled yes
            $replica config set maxmemory [expr {[s 0 used_memory] * 100 / 110}]
            wait_for_condition 50 100 {
                [getInfoProperty [$replica info throttling] ext_storage_throttle_fraction] == 1
            } else {
                fail_with_state "the replica's throttle didn't reach a full stop"
            }
            for {set i 0} {$i < 100} {incr i} {
                $primary set new:$i $val EX 100000
            }
            wait_for_condition 50 100 {
                [$replica dbsize] == 1100
            } else {
                fail_with_state "the primary's writes didn't reach the replica"
            }
            assert_equal 0 [getInfoProperty [$replica info throttling] ext_storage_throttle_queued_clients]
            $replica config set ext-storage-throttle-enabled no maxmemory 0
            $replica config set ext-storage-throttle-start-percent 105 ext-storage-throttle-max-percent 120
        }

        # The primary's writes don't count toward the baseline, so a replica
        # relies on ext-storage-throttle-min-baseline to keep serving.
        test "A replica just past the start still serves reads of values on flash ($engine)" {
            $primary set flashed [string repeat x 1000]
            wait_for_condition 50 100 {
                [$replica exists flashed] == 1
            } else {
                fail_with_state "the key didn't reach the replica"
            }
            $replica debug ext-storage spill flashed
            wait_for_flash flashed
            $replica config set ext-storage-throttle-enabled yes
            $replica config set maxmemory [expr {[s 0 used_memory] * 100 / 106}]
            wait_for_condition 50 100 {
                [getInfoProperty [$replica info throttling] ext_storage_throttle_fraction] > 0
            } else {
                fail_with_state "the replica's throttle didn't start"
            }
            set rd [valkey_deferring_client]
            $rd get flashed
            after 300
            assert_equal 0 [getInfoProperty [$replica info throttling] ext_storage_throttle_queued_clients]
            assert_equal [string repeat x 1000] [$rd read]
            $rd close
            $replica config set ext-storage-throttle-enabled no maxmemory 0
        } {OK} {needs:debug}
    }
    }
    file delete -force $primary_path $replica_path
  }
}
