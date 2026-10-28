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
    }
}

proc spilled_keys {} {
    return [getInfoProperty [r info ext_storage] ext_storage_total_num_items_spilled_to_storage]
}

# A server built without data tiering refuses to start with it enabled.
if {$tiering_built} {
    start_server {tags {"ext-storage external:skip"} overrides {ext-storage-enabled yes ext-storage-engine mock save ""}} {
        test {Keys over maxmemory are spilled instead of evicted} {
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
                fail "used_memory didn't drop below maxmemory"
            }
            assert_morethan [spilled_keys] 0
            assert_equal 0 [s evicted_keys]
            assert_equal 2200 [r dbsize]
            assert_equal PONG [r ping]
        }

        # Eviction skips keys on flash, so it must give up rather than resample
        # forever once nothing in memory can be evicted.
        test {Writes fail with OOM once nothing can be spilled or evicted} {
            r config set maxmemory 1
            set err {}
            for {set i 0} {$i < 1000} {incr i} {
                if {[catch {r set small:$i v} err]} break
            }
            assert_match {OOM*} $err
            r config set maxmemory [expr {[s used_memory] + 10000000}]
        }

        test {Saving fails while values are on flash} {
            catch {r save} e
            assert_match {*ERR*} $e
            r bgsave
            waitForBgsave r
            assert_equal err [s rdb_last_bgsave_status]
        }

        test {Spilled hashes and lists can be deleted and overwritten} {
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
                fail "used_memory didn't drop below maxmemory"
            }
            assert_morethan [spilled_keys] 0
            r config set maxmemory 0
            for {set i 0} {$i < 100} {incr i} {
                r del small_hash:$i big_hash:$i
                r set list:$i v
            }
            assert_equal 101 [r dbsize]
            assert_equal v [r get list:0]
        }

        # TODO(#39): reads and writes fetch the value instead.
        test {A key on flash reads as missing and a write starts from an empty key} {
            r flushall
            r config set maxmemory 0
            r config set maxmemory-policy allkeys-lru
            set val [string repeat x 1000]
            for {set i 0} {$i < 1000} {incr i} {
                r set key:$i $val
            }
            r config set maxmemory [expr {[s used_memory] - 200000}]
            r set trigger v
            wait_for_condition 50 100 {
                [spilled_keys] >= 2
            } else {
                fail "fewer than two keys were spilled"
            }
            r config set maxmemory 0

            set spilled {}
            for {set i 0} {$i < 1000 && [llength $spilled] < 2} {incr i} {
                if {[string match {* storage_state:2 *} [r debug object key:$i fast]]} {
                    lappend spilled key:$i
                }
            }
            lassign $spilled read write
            assert_equal {} [r get $read]
            assert_equal 0 [r exists $read]
            assert_equal 1 [r append $write v]
            assert_equal v [r get $write]
            assert_equal PONG [r ping]
        } {} {needs:debug}

        test {Keys over maxmemory are spilled under noeviction} {
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
                fail "no key was spilled"
            }
            assert_equal 0 [s evicted_keys]
            r config set maxmemory 0
            r config set maxmemory-policy allkeys-lru
        }

        test {Spilled keys can be flushed} {
            r flushall
            assert_equal 0 [r dbsize]
            r config set maxmemory 0
            r set key v
            assert_equal OK [r save]
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
                [spilled_keys] - $before >= 300
            } else {
                fail "the strings weren't all spilled"
            }
            r config set maxmemory 0
            r config set maxmemory-samples 5
            r config resetstat
        }

        foreach policy {allkeys-random allkeys-lru} {
            test "Eviction under $policy only takes keys in memory" {
                mostly_on_flash
                r config set maxmemory-policy $policy
                # One key per sample, so most samples hold only keys on flash.
                r config set maxmemory-samples 1
                r config set maxmemory [expr {[s used_memory] - 20000}]
                r set trigger v
                r config set maxmemory 0
                r config set maxmemory-samples 5
                r config set maxmemory-policy allkeys-lru
                assert_morethan [s evicted_keys] 0
                for {set i 0} {$i < 300} {incr i} {
                    assert_match {* storage_state:2 *} [r debug object flash:$i fast]
                }
            } {} {needs:debug}
        }
    }

    # Each replica decides what to spill from its own reads.
    start_server {tags {"ext-storage repl external:skip"} overrides {ext-storage-enabled yes ext-storage-engine mock save ""}} {
    start_server {overrides {ext-storage-enabled yes ext-storage-engine mock save ""}} {
        set primary [srv -1 client]
        set replica [srv 0 client]

        test {A replica spills on its own and never evicts} {
            set val [string repeat x 1000]
            for {set i 0} {$i < 1000} {incr i} {
                $primary set key:$i $val
            }
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up} && [$replica dbsize] == 1000
            } else {
                fail "replica didn't sync"
            }
            $replica config set maxmemory-policy allkeys-lru
            $replica config set maxmemory [expr {[s 0 used_memory] - 200000}]
            $replica ping
            wait_for_condition 50 100 {
                [getInfoProperty [$replica info ext_storage] ext_storage_total_num_items_spilled_to_storage] > 0
            } else {
                fail "replica didn't spill"
            }
            assert_equal 0 [s 0 evicted_keys]
            assert_equal 1000 [$replica dbsize]
            assert_equal 0 [getInfoProperty [$primary info ext_storage] ext_storage_total_num_items_spilled_to_storage]
        }
    }
    }
}
