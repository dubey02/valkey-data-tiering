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
    }
}
