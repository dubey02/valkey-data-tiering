set testmodule [file normalize tests/modules/storage_mock_module.so]

# Return the value of an INFO field, or "" if absent.
proc field {info property} {
    if {[regexp "\r\n$property:(.*?)\r\n" $info _ value]} {
        set _ $value
    }
}

# The module .so only builds with data tiering, so skip the whole suite when it
# is missing from a built-out tree.
if {![file exists $testmodule]} {
    start_server {} {
        test {storage engine module: skipped, built without data tiering} {
            assert_equal 1 1
        }
    }
    return
}

start_server [list tags {"modules" "ext-storage"} overrides [list loadmodule $testmodule ext-storage-enabled yes ext-storage-engine module_mock]] {
    test {module registers the storage engine} {
        verify_log_message 0 "*Module registered storage engine: module_mock*" 0
    }

    test {ext_storage INFO reports the module engine} {
        set info [r info ext_storage]
        assert_equal [field $info ext_storage_enabled] 1
        assert_equal [field $info ext_storage_engine] module_mock
        assert_equal [field $info ext_storage_api_version] 1
    }
}
