set hellostorage [file normalize tests/modules/hellostorage.so]

# INFO ext_storage is empty on a valkey server built without data tiering, and
# then there is nothing to test.
set built_in 0
start_server {} {
    set built_in [string match {*ext_storage_enabled:*} [r info ext_storage]]
}
if {!$built_in} {
    puts "Skipping unit/moduleapi/storage_engine: built without data tiering"
    return
}

# Each entry is a storage engine name and the module that provides it. The
# built-in mock needs no module.
set storage_engines [list mock {} hellostorage $hellostorage]

# Invariant under test: a storage engine is selected by ext-storage-engine and
# opened at startup the same way, whether built in or loaded from a module.
foreach {engine module} $storage_engines {
    set overrides [list ext-storage-enabled yes ext-storage-engine $engine]
    if {$module ne {}} {
        lappend overrides loadmodule $module
    }

    start_server [list tags {"modules" "ext-storage"} overrides $overrides] {
        if {$module ne {}} {
            test "$engine: module registers the storage engine" {
                verify_log_message 0 "*Module registered storage engine: $engine*" 0
            }
        }

        test "$engine: INFO ext_storage reports the storage engine" {
            set info [r info ext_storage]
            assert_equal [getInfoProperty $info ext_storage_enabled] 1
            assert_equal [getInfoProperty $info ext_storage_engine] $engine
            assert_equal [getInfoProperty $info ext_storage_api_version] 1
        }

        if {$module ne {}} {
            test "$engine: module that registered the storage engine cannot be unloaded" {
                assert_error "*registered a storage engine*" {r module unload $engine}
                assert_equal [getInfoProperty [r info ext_storage] ext_storage_engine] $engine
            }
        }
    }
}
