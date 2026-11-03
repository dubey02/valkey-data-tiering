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

# A valkey server built without data tiering refuses to start with it enabled.
if {$tiering_built} {
    start_server {tags {"ext-storage external:skip"} overrides {ext-storage-enabled yes ext-storage-engine mock save ""}} {
        test {DEBUG SPILL submits a key and refuses keys it can't spill} {
            r set big [string repeat x 1000]
            r set small v
            assert_equal OK [r debug spill big]
            # The completion is processed before the next command.
            assert_match {* storage_state:2 *} [r debug object big fast]
            # A key already on flash stays there.
            assert_equal OK [r debug spill big]
            assert_match {* storage_state:2 *} [r debug object big fast]
            # Too small to have a separate value, or missing.
            assert_error {*key 'small' can't be spilled*} {r debug spill small}
            assert_error {*No such key 'missing'*} {r debug spill missing}
        } {} {needs:debug}

        test {DEBUG SPILL submits several keys and stops at the first one that fails} {
            r flushall
            r set a [string repeat x 1000]
            r set b [string repeat x 1000]
            r set c [string repeat x 1000]
            assert_equal OK [r debug spill a b]
            assert_match {* storage_state:2 *} [r debug object a fast]
            assert_match {* storage_state:2 *} [r debug object b fast]
            # c is submitted before the missing key stops the command.
            assert_error {*No such key 'missing'*} {r debug spill c missing}
            assert_match {* storage_state:2 *} [r debug object c fast]
        } {} {needs:debug}

        test {DEBUG SPILL refuses more than 256 keys and submits none} {
            r flushall
            set keys {}
            for {set i 0} {$i < 257} {incr i} {
                r set key:$i [string repeat x 1000]
                lappend keys key:$i
            }
            assert_error {*at most 256 keys*} {r debug spill {*}$keys}
            assert_match {* storage_state:0 *} [r debug object key:0 fast]
            assert_equal OK [r debug spill {*}[lrange $keys 0 255]]
            # The storage engine completes a batch per event loop cycle.
            wait_for_condition 100 50 {
                [getInfoProperty [r info ext_storage] ext_storage_num_items_spilling_to_storage] == 0
            } else {
                puts [r info ext_storage]
                fail "spills didn't complete"
            }
            assert_match {* storage_state:2 *} [r debug object key:255 fast]
        } {} {needs:debug}

        test {DEBUG commands that read a value fail on a key on flash} {
            r flushall
            r set str [string repeat x 1000]
            # Over list-max-listpack-size, so it's a quicklist.
            r rpush list [string repeat x 10000]
            # Over hash-max-listpack-value, so it's a hashtable.
            r hset hash f [string repeat x 1000]
            r set mem v
            set mem_digest [lindex [r debug digest-value mem] 0]
            assert_equal OK [r debug spill str list hash]

            assert_error {*value of 'str' isn't in memory*} {r debug sdslen str}
            assert_error {*value of 'str' isn't in memory*} {r debug listpack str}
            assert_error {*value of 'list' isn't in memory*} {r debug quicklist list}
            assert_error {*value of 'hash' isn't in memory*} {r debug htstats-key hash}
            assert_error {*value of 'list' isn't in memory*} {r debug digest-value mem list}
            assert_error {*value isn't in memory*} {r debug digest}
            # A key in memory still works.
            assert_equal $mem_digest [lindex [r debug digest-value mem] 0]
            assert_match {*val_sds_len:1*} [r debug sdslen mem]
        } {} {needs:debug}

        test {DEBUG OBJECT reports a key on flash without reading its value} {
            r flushall
            # Over list-max-listpack-size, so it's a quicklist.
            r rpush list [string repeat x 10000]
            assert_equal OK [r debug spill list]
            set info [r debug object list]
            assert_match {* storage_state:2 *} $info
            assert_no_match {*serializedlength*} $info
            assert_no_match {*ql_nodes*} $info
        } {} {needs:debug}

        test {A command on a key on flash fetches it back into memory} {
            r flushall
            set val [string repeat x 1000]
            r set big $val
            r set mem v

            assert_equal OK [r debug spill big]
            assert_equal $val [r get big]
            assert_match {* storage_state:0 *} [r debug object big fast]

            assert_equal OK [r debug spill big]
            assert_equal 1001 [r append big y]
            assert_equal ${val}y [r get big]

            assert_equal OK [r debug spill big]
            assert_equal [list v ${val}y] [r mget mem big]

            assert_equal OK [r debug spill big]
            assert_equal 1 [r del big]
            assert_equal 0 [r exists big]
        } {} {needs:debug}

        # Active defrag needs the jemalloc the source distribution ships, as in memefficiency.tcl.
        if {[string match {*jemalloc*} [s mem_allocator]] && [r debug mallctl arenas.page] <= 8192} {
            test {Active defrag skips keys on flash} {
                r flushall
                for {set i 0} {$i < 100} {incr i} {
                    r set key:$i [string repeat x 1000]
                    r rpush list:$i [string repeat x 1000]
                    assert_equal OK [r debug spill key:$i]
                    assert_equal OK [r debug spill list:$i]
                    # Keys in memory, so the scan visibly makes progress.
                    r set mem:$i [string repeat x 1000]
                }
                set visited [expr {[s active_defrag_key_hits] + [s active_defrag_key_misses]}]
                r config set active-defrag-ignore-bytes 1
                r config set active-defrag-threshold-lower 0
                r config set activedefrag yes
                # Wait for a full pass over the keys in memory, or for defrag to finish.
                wait_for_condition 100 50 {
                    [set delta [expr {[s active_defrag_key_hits] + [s active_defrag_key_misses] - $visited}]] >= 100 ||
                    ($delta > 0 && [s active_defrag_running] eq 0)
                } else {
                    r config set activedefrag no
                    fail "active defrag didn't run"
                }
                r config set activedefrag no
                assert_equal PONG [r ping]
                assert_match {* storage_state:2 *} [r debug object list:0 fast]
            } {} {needs:debug}
        }
    }
}

start_server {tags {"ext-storage external:skip"}} {
    if {[string match {*ext_storage_enabled:*} [r info ext_storage]]} {
        test {DEBUG SPILL needs data tiering enabled} {
            r set big [string repeat x 1000]
            assert_error {*not enabled*} {r debug spill big}
        } {} {needs:debug}
    }
}
