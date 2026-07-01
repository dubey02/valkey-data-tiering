# Regression test for embstr restore UAF (ext_storage.c:733-741).
#
# With ASan: immediate heap-use-after-free on first GET of a spilled key
# whose value restores as EMBSTR (≤128B standalone).
#
# Run:
#   make SANITIZER=address
#   fallocate -l 256M /tmp/valkey-flash-uaf.db
#   EXT_STORAGE_BACKEND=flashcache EXT_STORAGE_PATH=/tmp/valkey-flash-uaf.db \
#     ./runtest --single tests/unit/ext-storage-embstr-spill.tcl

proc get_spill_count {} {
    set info [r info all]
    if {[regexp {total_num_items_spilled_to_ext_storage:(\d+)} $info _ val]} { return $val }
    return 0
}
proc debug_spill {key {timeout 5000}} {
    set before [get_spill_count]
    r debug spill $key
    set start [clock milliseconds]
    while {1} {
        if {[get_spill_count] > $before} { return }
        if {[clock milliseconds] - $start > $timeout} { error "spill timeout" }
        after 50
    }
}

if {[info exists ::env(EXT_STORAGE_BACKEND)]} {
    set _backend $::env(EXT_STORAGE_BACKEND)
} else {
    set _backend "flashcache-mock"
}
if {[info exists ::env(EXT_STORAGE_PATH)]} {
    set _path $::env(EXT_STORAGE_PATH)
} else {
    set _path "/tmp/valkey-flash-uaf-[pid].db"
}

start_server [list tags {"ext-storage-embstr"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_path \
    ext-storage-capacity-mb 256 \
    maxmemory 10mb \
    maxmemory-policy allkeys-lru \
]] {
    test {Embstr restore UAF: spill 64B key + 100B value then GET} {
        # 64B key + 100B value = 181B total robj → RAW (spillable).
        # On fetch, value alone = 112B ≤ 128 → EMBSTR restore → UAF.
        # Requires SANITIZER=address build to detect: without ASan the
        # dangling read returns intact data and the test passes silently.
        set key [string repeat "K" 64]
        set val [string repeat "V" 100]

        r set $key $val
        debug_spill $key

        # This GET triggers the UAF. With ASan: immediate fault.
        set fetched [r get $key]
        assert_equal $fetched $val
    }
}
