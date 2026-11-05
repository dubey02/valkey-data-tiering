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

# A valkey server built without data tiering refuses to start with it enabled.
if {$tiering_built} {
  foreach engine {mock flashcache} {
    # The valkey server runs in its own directory, so the path is absolute.
    set path [file normalize [tmpdir ext-storage-$engine]]
    start_server [list tags {"ext-storage external:skip"} overrides [list ext-storage-enabled yes ext-storage-engine $engine ext-storage-path $path ext-storage-capacity 128mb save ""]] {
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
            # The storage engine completes a batch per event loop cycle.
            wait_for_condition 100 50 {
                [getInfoProperty [r info ext_storage] ext_storage_num_items_spilling_to_storage] == 0
            } else {
                puts [r info ext_storage]
                puts [exec tail -n 20 < [srv 0 stdout]]
                fail "spills didn't complete"
            }
            assert_match {* storage_state:2 *} [r debug object key:255 fast]
        } {} {needs:debug}

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

        test "A command on a key on flash fetches it back into memory ($engine)" {
            r flushall
            set val [string repeat x 1000]
            r set big $val
            r set mem v

            assert_equal OK [r debug ext-storage spill big]
            wait_for_flash big
            assert_equal $val [r get big]
            assert_match {* storage_state:0 *} [r debug object big fast]

            assert_equal OK [r debug ext-storage spill big]
            wait_for_flash big
            assert_equal 1001 [r append big y]
            assert_equal ${val}y [r get big]

            assert_equal OK [r debug ext-storage spill big]
            wait_for_flash big
            assert_equal [list v ${val}y] [r mget mem big]

            assert_equal OK [r debug ext-storage spill big]
            wait_for_flash big
            assert_equal 1 [r del big]
            assert_equal 0 [r exists big]
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

# Prints the storage state and the end of the log of valkey server 'level',
# then fails.
proc fail_with_state {msg {level 0}} {
    puts [r $level info ext_storage]
    puts [exec tail -n 20 < [srv $level stdout]]
    fail $msg
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
    set path [file normalize [tmpdir ext-storage-$engine]]
    start_server [list tags {"ext-storage external:skip"} overrides [list ext-storage-enabled yes ext-storage-engine $engine ext-storage-path $path ext-storage-capacity 128mb save ""]] {
        test "Keys over maxmemory are spilled instead of evicted ($engine)" {
            r config set maxmemory-policy allkeys-lru
            set val [string repeat x 1000]
            for {set i 0} {$i < 2000} {incr i} {
                r set key:$i $val
            }
            r config set maxmemory [expr {[s used_memory] - 500000}]
            for {set i 0} {$i < 200} {incr i} {
                r set new:$i $val
            }
            wait_for_condition 50 100 {
                [s used_memory] <= [s maxmemory]
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
            r config set maxmemory [expr {[s used_memory] - 500000}]
            r set trigger $val
            wait_for_condition 50 100 {
                [s used_memory] <= [s maxmemory]
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
            r config set maxmemory [expr {[s used_memory] - 200000}]
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

        foreach policy {allkeys-random allkeys-lru allkeys-lfu} {
            test "Eviction under $policy only takes keys in memory ($engine)" {
                mostly_on_flash
                # An LFU maxmemory-policy needs spilling to rank by LFU too.
                set spill_policy [expr {$policy eq "allkeys-lfu" ? "allkeys-lfu" : "allkeys-lru"}]
                r config set maxmemory-policy $policy ext-storage-spill-policy $spill_policy
                # One key per sample, so most samples hold only keys on flash. A
                # sample with only keys on flash gives up with OOM, so retry.
                r config set maxmemory-samples 1
                r config set maxmemory [expr {[s used_memory] - 20000}]
                for {set i 0} {$i < 100 && [s evicted_keys] == 0} {incr i} {
                    catch {r set trigger v}
                }
                r config set maxmemory 0
                r config set maxmemory-samples 5
                r config set maxmemory-policy allkeys-lru ext-storage-spill-policy allkeys-lru
                assert_morethan [s evicted_keys] 0
                for {set i 0} {$i < 300} {incr i} {
                    assert_match {* storage_state:2 *} [r debug object flash:$i fast]
                }
                assert_equal [string repeat x 1000] [r get flash:0]
            } {} {needs:debug}
        }
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
    }
    }
    file delete -force $primary_path $replica_path
  }
}
