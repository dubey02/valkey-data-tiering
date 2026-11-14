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
    }
    file delete -force $path
  }
}
