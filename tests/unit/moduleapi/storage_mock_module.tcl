set testmodule [file normalize tests/modules/storage_mock_module.so]

# The module .so only builds with data tiering. On a built-out tree there is
# nothing to load, so the suite has no tests to run.
if {![file exists $testmodule]} {
    puts "Skipping unit/moduleapi/storage_mock_module: built without data tiering"
    return
}

# Invariant under test: a storage engine registered from a module is selected by
# ext-storage-engine and opened at startup exactly like a built-in one.
start_server [list tags {"modules" "ext-storage"} overrides [list loadmodule $testmodule ext-storage-enabled yes ext-storage-engine module_mock]] {
    test {module registers the storage engine} {
        verify_log_message 0 "*Module registered storage engine: module_mock*" 0
    }

    test {ext_storage INFO reports the module storage engine} {
        set info [r info ext_storage]
        assert_equal [getInfoProperty $info ext_storage_enabled] 1
        assert_equal [getInfoProperty $info ext_storage_engine] module_mock
        assert_equal [getInfoProperty $info ext_storage_api_version] 1
    }
}
