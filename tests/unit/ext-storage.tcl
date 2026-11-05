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

        test {DEBUG SPILL needs data tiering enabled} {
            r set big [string repeat x 1000]
            assert_error {*not enabled*} {r debug spill big}
        } {} {needs:debug external:skip}
    }
}

# A storage engine may finish a spill after DEBUG SPILL returns, so this waits
# until each key's value is on flash.
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
        test "DEBUG SPILL submits a key and refuses keys it can't spill ($engine)" {
            r set big [string repeat x 1000]
            r set small v
            assert_equal OK [r debug spill big]
            wait_for_flash big
            # A key already on flash stays there.
            assert_equal OK [r debug spill big]
            assert_match {* storage_state:2 *} [r debug object big fast]
            # A short value is embedded in the key, so it can't be spilled.
            assert_error {*key 'small' can't be spilled*} {r debug spill small}
        } {} {needs:debug}

        test "DEBUG SPILL submits several keys and stops at the first one that fails ($engine)" {
            r flushall
            r set a [string repeat x 1000]
            r set b [string repeat x 1000]
            r set c [string repeat x 1000]
            assert_equal OK [r debug spill a b]
            wait_for_flash a b
            # c is submitted before the missing key stops the command.
            assert_error {*No such key 'missing'*} {r debug spill c missing}
            wait_for_flash c
        } {} {needs:debug}

        test "DEBUG SPILL refuses more than 256 keys and submits none ($engine)" {
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
            assert_equal OK [r debug spill str list hash]
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
            assert_equal OK [r debug spill list]
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

            assert_equal OK [r debug spill big]
            wait_for_flash big
            assert_equal $val [r get big]
            assert_match {* storage_state:0 *} [r debug object big fast]

            assert_equal OK [r debug spill big]
            wait_for_flash big
            assert_equal 1001 [r append big y]
            assert_equal ${val}y [r get big]

            assert_equal OK [r debug spill big]
            wait_for_flash big
            assert_equal [list v ${val}y] [r mget mem big]

            assert_equal OK [r debug spill big]
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
                    assert_equal OK [r debug spill key:$i]
                    assert_equal OK [r debug spill list:$i]
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
