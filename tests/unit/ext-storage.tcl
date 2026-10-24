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

# A server built without data tiering refuses to start with it enabled.
if {$tiering_built} {
    start_server {tags {"ext-storage external:skip"} overrides {ext-storage-enabled yes ext-storage-engine mock save ""}} {
        test {DEBUG SPILL submits a key and refuses keys it can't spill} {
            r set big [string repeat x 1000]
            r set small v
            assert_equal OK [r debug spill big]
            assert_match {* storage_state:1 *} [r debug object big fast]
            # Already being written, too small to have a separate value, or missing.
            assert_error {*can't be spilled*} {r debug spill big}
            assert_error {*can't be spilled*} {r debug spill small}
            assert_error {*no such key*} {r debug spill missing}
        } {} {needs:debug}

        test {DEBUG SPILL refuses a value over ext-storage-max-spill-size} {
            r set large [string repeat x 2000]
            r config set ext-storage-max-spill-size 1000
            assert_error {*can't be spilled*} {r debug spill large}
            r config set ext-storage-max-spill-size 128mb
            assert_equal OK [r debug spill large]
        } {} {needs:debug}
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
