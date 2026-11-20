set testmodule [file normalize tests/modules/datatype.so]

# Stays 0 when a tag filter skips the block that checks.
set tiering_built 0
start_server {tags {"modules ext-storage"}} {
    # INFO ext_storage is empty on a server built without data tiering.
    set tiering_built [string match {*ext_storage_enabled:*} [r info ext_storage]]
}

proc wait_for_flash {key} {
    wait_for_condition 100 50 {
        [string match {* storage_state:2 *} [r debug object $key fast]]
    } else {
        puts [r info ext_storage]
        puts [exec tail -n 20 < [srv 0 stdout]]
        fail "$key didn't reach flash"
    }
}

if {$tiering_built} {
  foreach engine {mock flashcache} {
    # The valkey server runs in its own directory, so the path is absolute.
    set path [file normalize [tmpdir ext-storage-$engine]]
    start_server [list tags {"modules ext-storage external:skip"} overrides [list ext-storage-enabled yes ext-storage-engine $engine ext-storage-path $path ext-storage-capacity 128mb save ""]] {
        r module load $testmodule

        foreach lazy {no yes} {
            test "A module value is spilled and fetched back with the same value, lazyfree-lazy-server-del $lazy ($engine)" {
                r flushall
                r config set lazyfree-lazy-server-del $lazy
                set val [string repeat x 1000]
                r datatype.set dtkey 100 $val
                set spilled [getInfoProperty [r info ext_storage] ext_storage_total_num_items_spilled_to_storage]

                assert_equal OK [r debug ext-storage spill dtkey]
                wait_for_flash dtkey
                assert_equal [expr {$spilled + 1}] [getInfoProperty [r info ext_storage] ext_storage_total_num_items_spilled_to_storage]
                assert_equal [list 100 $val] [r datatype.get dtkey]
                assert_match {* storage_state:0 *} [r debug object dtkey fast]

                # A write fetches the value first, then replaces it.
                assert_equal OK [r debug ext-storage spill dtkey]
                wait_for_flash dtkey
                r datatype.set dtkey 200 newval
                assert_equal {200 newval} [r datatype.get dtkey]

                assert_equal OK [r debug ext-storage spill dtkey]
                wait_for_flash dtkey
                assert_equal 1 [r del dtkey]
                assert_equal 0 [r exists dtkey]
            } {} {needs:debug}
        }

        test "SCAN TYPE skips a module key on flash and still matches core keys on flash ($engine)" {
            r flushall
            r datatype.set dtkey 100 [string repeat x 1000]
            r datatype.set dtmem 1 v
            r set str [string repeat x 1000]
            assert_equal OK [r debug ext-storage spill dtkey str]
            wait_for_flash dtkey
            wait_for_flash str
            assert_equal {dtmem} [lsort [lindex [r scan 0 count 100 type test___dt] 1]]
            assert_equal {str} [lindex [r scan 0 count 100 type string] 1]
            # The module key on flash still reads back.
            assert_equal 100 [lindex [r datatype.get dtkey] 0]
        } {} {needs:debug}

        test "A module that reads the dbid in rdb_save and rdb_load gets the key's database ($engine)" {
            r flushall
            r module load [file normalize tests/modules/datatype2.so]
            r select 9
            assert_equal 1 [r mem.alloc k 1]
            assert_equal 5 [r mem.write k 0 hello]
            assert_equal OK [r debug ext-storage spill k]
            wait_for_flash k
            assert_equal hello [string trimright [r mem.read k 0] "\0"]
            assert_equal OK [r select 0]
        } {} {needs:debug}

        test "A module that gets a context in rdb_load can spill and fetch ($engine)" {
            r flushall
            r module load [file normalize tests/modules/testrdb.so]
            r testrdb.set.key k [string repeat x 1000]
            assert_equal OK [r debug ext-storage spill k]
            wait_for_flash k
            assert_equal [string repeat x 1000] [r testrdb.get.key k]
        } {} {needs:debug}

        test "A module value stays in memory unless its module opts in ($engine)" {
            r flushall
            r module load [file normalize tests/modules/test_lazyfree.so]
            r lazyfreelink.insert k 1
            assert_error {*can't be spilled*} {r debug ext-storage spill k}
            assert_match {* storage_state:0 *} [r debug object k fast]
            assert_equal 1 [r lazyfreelink.len k]
        } {} {needs:debug}

        test "Module APIs see a key on flash as missing outside a command ($engine)" {
            r flushall
            r module load [file normalize tests/modules/scan.so]
            r module load [file normalize tests/modules/blockedclient.so]
            r set mem v
            r set str [string repeat x 1000]
            r hset hash f [string repeat x 1000]
            r datatype.set dtkey 100 [string repeat x 1000]
            assert_equal OK [r debug ext-storage spill str hash dtkey]
            wait_for_flash str
            wait_for_flash hash
            wait_for_flash dtkey
            # ValkeyModule_Scan skips keys on flash.
            assert_equal {{mem v}} [r scan.scan_strings]
            # ValkeyModule_OpenKey on a key the command didn't declare.
            assert_error {*not found*} {r scan.scan_key hash}
            # RM_Call skips the fetch a client command gets.
            assert_equal {} [r do_rm_call datatype.get dtkey]
            # A write open fails, so the value on flash isn't overwritten.
            assert_error {*can't be opened for writing*} {r do_rm_call datatype.set dtkey 7 new}
            assert_equal PONG [r ping]
            # A client command still fetches and reads them.
            assert_equal [string repeat x 1000] [r get str]
            assert_equal [string repeat x 1000] [r hget hash f]
            assert_equal 100 [lindex [r datatype.get dtkey] 0]
        } {} {needs:debug}
    }
    file delete -force $path
  }
}
