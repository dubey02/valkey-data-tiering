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

# A storage engine may finish a spill after DEBUG EXT-STORAGE SPILL returns, so
# this waits until each key's value is on flash.
proc wait_for_flash {args} {
    foreach key $args {
        wait_for_condition 100 50 {
            [string match {* storage_state:2 *} [r debug object $key fast]]
        } else {
            puts [r info ext_storage]
            fail "$key didn't reach flash"
        }
    }
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

        test "$engine: spilled values come back from the storage engine" {
            set keys {}
            for {set i 0} {$i < 20} {incr i} {
                r set key:$i [string repeat v$i 100]
                r debug ext-storage spill key:$i
                lappend keys key:$i
            }
            wait_for_flash {*}$keys
            for {set i 0} {$i < 20} {incr i} {
                assert_equal [string repeat v$i 100] [r get key:$i]
            }
            set info [r info ext_storage]
            assert_morethan_equal [getInfoProperty $info ext_storage_total_num_items_spilled_to_storage] 20
            assert_morethan_equal [getInfoProperty $info ext_storage_total_num_items_fetched_from_storage] 20
        } {} {needs:debug}

        test "$engine: a spilled key can be deleted and overwritten" {
            r set gone [string repeat g 1000]
            r set kept [string repeat k 1000]
            r debug ext-storage spill gone
            r debug ext-storage spill kept
            wait_for_flash gone kept
            assert_equal 1 [r del gone]
            assert_equal 0 [r exists gone]
            r set kept new
            assert_equal new [r get kept]
        } {} {needs:debug}

        if {$module ne {}} {
            test "$engine: module that registered the storage engine cannot be unloaded" {
                assert_error "*registered a storage engine*" {r module unload $engine}
                assert_equal [getInfoProperty [r info ext_storage] ext_storage_engine] $engine
            }
        }
    }
}

# Invariant under test: the storage engine registration is dropped when the
# module that made it fails OnLoad, so another module can register.
start_server {tags {"modules" "ext-storage"}} {
    test {failed OnLoad drops the storage engine registration} {
        assert_error "*module initialization failed*" {r module load $hellostorage noload}
        assert_equal {OK} [r module load $hellostorage]
    }
}

# Invariant under test: a module that fails to load without registering a
# storage engine leaves the registered one alone.
set hooks [file normalize tests/modules/hooks.so]
start_server [list tags {"modules" "ext-storage"} overrides [list loadmodule $hellostorage ext-storage-enabled yes ext-storage-engine hellostorage]] {
    test {a failed load of another module keeps the storage engine registration} {
        assert_error "*module initialization failed*" {r module load $hooks noload}
        assert_error "*registered a storage engine*" {r module unload hellostorage}
        assert_equal [getInfoProperty [r info ext_storage] ext_storage_engine] hellostorage
    }
}
