start_server {tags {"ext-storage"}} {
    # INFO ext_storage is empty on a server built without data tiering.
    if {[string match {*ext_storage_enabled:*} [r info ext_storage]]} {
        test {DEBUG OBJECT reports storage_state for in-memory keys} {
            r flushdb
            r set str v
            r rpush list a b
            r hset hash f v
            foreach key {str list hash} {
                assert_match {* storage_state:0 *} [r debug object $key]
            }
        } {} {needs:debug}

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
