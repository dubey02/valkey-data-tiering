# Ext-storage admission/promotion policy matrix integration tests.
#
# Tests the config surface and observable behavior of:
#   ext-storage-admission-policy  (dram | flash)
#   ext-storage-promotion-policy  (always | never | 2hit-50k)
#
# Section 9 (2hit-50k) uses the native flashcache-mock backend compiled into
# valkey-server and does NOT require any external module or NVMe device.
#
# Sections 1-8 require the key-spilling module to be built:
#   cd modules/key-spilling && cargo build --release
#
# Run with:
#   ./runtest --single tests/unit/ext-storage-policies.tcl

proc get_info_field {field} {
    set info [r info all]
    if {[regexp "${field}:(\\S+)" $info _ val]} {
        return $val
    }
    return ""
}

proc wait_for_spill_complete {key {timeout 5000}} {
    # Wait until a DEBUG SPILL completes (item transitions to ONLY_FLASH).
    # We detect this by num_items_spilling_to_ext_storage dropping back to 0
    # and num_items_on_flash incrementing.
    set start [clock milliseconds]
    while {1} {
        set spilling [get_info_field "num_items_spilling_to_ext_storage"]
        if {$spilling == 0} {
            return
        }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill of $key to complete"
        }
        after 10
    }
}

proc wait_for_flash_admit {expected_min {timeout 5000}} {
    # Wait until flash_admit_writes reaches at least expected_min.
    set start [clock milliseconds]
    while {1} {
        set val [get_info_field "flash_admit_writes"]
        if {$val >= $expected_min} {
            return $val
        }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for flash_admit_writes (got $val, wanted $expected_min)"
        }
        after 10
    }
}

# ============================================================================
# Section 9: 2hit-50k Promotion Policy (native flashcache-mock, no module)
#
# Uses the native flashcache-mock backend (compiled into valkey-server) — no
# external module or NVMe device required. The server defaults to
# "flashcache-mock" when ext-storage-backend is not set.
# ============================================================================

start_server [list tags {"ext-storage"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache-mock \
    maxmemory 8mb \
    maxmemory-policy allkeys-lru \
]] {

    # --- 9a: CONFIG surface for 2hit-50k ---

    test {CONFIG GET ext-storage-promotion-policy defaults to always (2hit section)} {
        set val [lindex [r config get ext-storage-promotion-policy] 1]
        assert_equal $val "always"
    }

    test {CONFIG SET ext-storage-promotion-policy accepts 2hit-50k} {
        r config set ext-storage-promotion-policy 2hit-50k
        set val [lindex [r config get ext-storage-promotion-policy] 1]
        assert_equal $val "2hit-50k"
    }

    test {CONFIG SET ext-storage-promotion-policy still accepts always after 2hit-50k} {
        r config set ext-storage-promotion-policy always
        set val [lindex [r config get ext-storage-promotion-policy] 1]
        assert_equal $val "always"
    }

    test {CONFIG SET ext-storage-promotion-policy rejects invalid value (2hit section)} {
        assert_error {*ERR*} {r config set ext-storage-promotion-policy invalid-policy}
    }

    # --- 9b: 2hit-50k behavioral tests ---

    test {2hit-50k: 1st GET serves value transiently, key stays on flash} {
        r flushall
        r config set ext-storage-admission-policy dram
        r config set ext-storage-promotion-policy 2hit-50k

        # Write a spillable key and force it to flash
        set val "twohit_value_[string repeat X 512]"
        r set twohit:k1 $val
        r debug spill twohit:k1
        wait_for_spill_complete twohit:k1

        set on_flash_before [get_info_field "num_items_on_flash"]
        assert {$on_flash_before >= 1}

        set filtered_before [get_info_field "promotion_filtered_onehit"]
        set admitted_before [get_info_field "promotion_2hit_admitted"]

        # 1st GET — should return correct value
        set got [r get twohit:k1]
        assert_equal $got $val

        # Wait for async completion
        after 300

        # promotion_filtered_onehit should increment
        set filtered_after [get_info_field "promotion_filtered_onehit"]
        assert {$filtered_after > $filtered_before}

        # promotion_2hit_admitted should NOT increment
        set admitted_after [get_info_field "promotion_2hit_admitted"]
        assert_equal $admitted_after $admitted_before

        # Key should still be on flash (transient serve, then reverted)
        after 300
        set on_flash_after [get_info_field "num_items_on_flash"]
        assert {$on_flash_after >= $on_flash_before}
    }

    test {2hit-50k: 2nd GET promotes key permanently (admitted)} {
        # Continue with twohit:k1 still on flash from previous test
        set val "twohit_value_[string repeat X 512]"

        set admitted_before [get_info_field "promotion_2hit_admitted"]
        set on_flash_before [get_info_field "num_items_on_flash"]

        # 2nd GET of the same key (within 50k window)
        set got [r get twohit:k1]
        assert_equal $got $val

        # Wait for async completion
        after 300

        # promotion_2hit_admitted should increment
        set admitted_after [get_info_field "promotion_2hit_admitted"]
        assert {$admitted_after > $admitted_before}

        # Key should no longer be on flash (promoted permanently)
        set on_flash_after [get_info_field "num_items_on_flash"]
        assert {$on_flash_after < $on_flash_before}
    }

    test {2hit-50k: one-hit-wonder key stays on flash (not promoted)} {
        r flushall
        r config set ext-storage-admission-policy dram
        r config set ext-storage-promotion-policy 2hit-50k

        # Write and spill a key
        set val "onehit_wonder_[string repeat W 512]"
        r set onehit:k1 $val
        r debug spill onehit:k1
        wait_for_spill_complete onehit:k1

        set on_flash_before [get_info_field "num_items_on_flash"]
        assert {$on_flash_before >= 1}

        set admitted_before [get_info_field "promotion_2hit_admitted"]

        # Single GET only — one-hit-wonder
        set got [r get onehit:k1]
        assert_equal $got $val

        # Wait for async
        after 300

        # Key should stay on flash — NOT promoted
        set on_flash_after [get_info_field "num_items_on_flash"]
        assert {$on_flash_after >= $on_flash_before}

        # promotion_2hit_admitted should NOT have incremented
        set admitted_after [get_info_field "promotion_2hit_admitted"]
        assert_equal $admitted_after $admitted_before
    }

    test {2hit-50k: value byte-correctness on both GETs} {
        r flushall
        r config set ext-storage-admission-policy dram
        r config set ext-storage-promotion-policy 2hit-50k

        # Use a distinct value pattern for byte-correctness
        set val "EXACT_BYTES_[string repeat ABCDEFGH 64]"
        r set bytecheck:k1 $val
        r debug spill bytecheck:k1
        wait_for_spill_complete bytecheck:k1

        # 1st GET — verify byte-for-byte correctness
        set got1 [r get bytecheck:k1]
        assert_equal $got1 $val

        after 300

        # 2nd GET — verify byte-for-byte correctness (now promoted)
        set got2 [r get bytecheck:k1]
        assert_equal $got2 $val
    }
}

# ============================================================================
# Sections 1-8: Module-based tests (require key-spilling module)
# ============================================================================

set testmodule [file normalize modules/key-spilling/target/release/libkey_spilling_module.so]

# Skip remaining sections if module not built
if {![file exists $testmodule]} {
    puts "SKIPPED (Sections 1-8): key-spilling module not built at $testmodule"
    puts "Build with: cd modules/key-spilling && cargo build --release"
    return
}

# ============================================================================
# Section 1: CONFIG plumbing (no backend needed — just config surface tests)
# ============================================================================

start_server [list tags {"ext-storage"} overrides [list \
    ext-storage-enabled yes \
    maxmemory 2mb \
    maxmemory-policy allkeys-lru \
    loadmodule "$testmodule backend=rocksdb db_path=/tmp/valkey-test-policies-[pid]" \
]] {

    test {CONFIG GET ext-storage-admission-policy defaults to dram} {
        set val [lindex [r config get ext-storage-admission-policy] 1]
        assert_equal $val "dram"
    }

    test {CONFIG GET ext-storage-promotion-policy defaults to always} {
        set val [lindex [r config get ext-storage-promotion-policy] 1]
        assert_equal $val "always"
    }

    test {CONFIG SET ext-storage-admission-policy accepts flash} {
        r config set ext-storage-admission-policy flash
        set val [lindex [r config get ext-storage-admission-policy] 1]
        assert_equal $val "flash"
    }

    test {CONFIG SET ext-storage-admission-policy accepts dram} {
        r config set ext-storage-admission-policy dram
        set val [lindex [r config get ext-storage-admission-policy] 1]
        assert_equal $val "dram"
    }

    test {CONFIG SET ext-storage-promotion-policy accepts never} {
        r config set ext-storage-promotion-policy never
        set val [lindex [r config get ext-storage-promotion-policy] 1]
        assert_equal $val "never"
    }

    test {CONFIG SET ext-storage-promotion-policy accepts always} {
        r config set ext-storage-promotion-policy always
        set val [lindex [r config get ext-storage-promotion-policy] 1]
        assert_equal $val "always"
    }

    test {CONFIG SET ext-storage-admission-policy rejects invalid value} {
        assert_error {*ERR*} {r config set ext-storage-admission-policy bogus}
    }

    test {CONFIG SET ext-storage-promotion-policy rejects invalid value} {
        assert_error {*ERR*} {r config set ext-storage-promotion-policy bogus}
    }

    test {CONFIG SET ext-storage-admission-policy is runtime-modifiable} {
        # Verify we can toggle at runtime without restart
        r config set ext-storage-admission-policy flash
        assert_equal [lindex [r config get ext-storage-admission-policy] 1] "flash"
        r config set ext-storage-admission-policy dram
        assert_equal [lindex [r config get ext-storage-admission-policy] 1] "dram"
    }

    test {CONFIG SET ext-storage-promotion-policy is runtime-modifiable} {
        r config set ext-storage-promotion-policy never
        assert_equal [lindex [r config get ext-storage-promotion-policy] 1] "never"
        r config set ext-storage-promotion-policy always
        assert_equal [lindex [r config get ext-storage-promotion-policy] 1] "always"
    }

    # ========================================================================
    # Section 2: Admission dram (default) — value resident after first write
    # ========================================================================

    test {Admission dram: new key value is resident after first write} {
        r config set ext-storage-admission-policy dram
        # Use a value above the embstr threshold (~44 bytes) to be spillable
        set padding [string repeat "X" 256]
        r set admtest:dram:k1 "val_${padding}"

        # Key should be present and retrievable immediately (in memory)
        set val [r get admtest:dram:k1]
        assert {[string match "val_*" $val]}

        # num_items_on_flash should NOT have incremented for this key
        # (value is resident in DRAM, not flash-admitted)
        set flash_admits [get_info_field "flash_admit_writes"]
        # flash_admit_writes should be 0 since we're using admission=dram
        assert {$flash_admits == 0}
    }

    # ========================================================================
    # Section 3: Admission flash — new key admitted to flash on first write
    # ========================================================================

    test {Admission flash: new key is flash-admitted on first write} {
        r config set ext-storage-admission-policy flash

        set flash_before [get_info_field "flash_admit_writes"]
        set padding [string repeat "Y" 256]
        r set admtest:flash:k1 "flashval_${padding}"

        # Wait for flash_admit_writes to increment
        wait_for_flash_admit [expr {$flash_before + 1}]

        set flash_after [get_info_field "flash_admit_writes"]
        assert {$flash_after > $flash_before}

        # GET should still return the correct value (fetched from flash or
        # served from in-flight buffer)
        set val [r get admtest:flash:k1]
        assert {[string match "flashval_*" $val]}
    }

    test {Admission flash: RMW on existing key does NOT flash-admit} {
        r config set ext-storage-admission-policy flash

        # Write initial key
        set padding [string repeat "Z" 256]
        r set admtest:flash:k2 "initial_${padding}"
        # Wait for flash admit of the create
        after 200

        set flash_before [get_info_field "flash_admit_writes"]

        # RMW: APPEND to existing key — should NOT trigger flash admission
        r append admtest:flash:k2 "_appended"

        # Give time for any async processing
        after 200

        set flash_after [get_info_field "flash_admit_writes"]
        # flash_admit_writes should NOT have incremented for the APPEND
        assert_equal $flash_after $flash_before
    }

    test {Admission flash: DBSIZE includes flash-admitted keys} {
        r config set ext-storage-admission-policy flash
        r flushall

        set padding [string repeat "A" 256]
        for {set i 0} {$i < 10} {incr i} {
            r set "admtest:flash:batch:$i" "v${i}_${padding}"
        }

        # Wait for flash admits to process
        wait_for_flash_admit 10

        # All keys should be visible in DBSIZE (non-key-spilling: keys stay in dict)
        set dbsize [r dbsize]
        assert_equal $dbsize 10
    }

    # ========================================================================
    # Section 4: Promotion always (default) — spilled key promoted on fetch
    # ========================================================================

    test {Promotion always: fetching a spilled key promotes it resident} {
        r flushall
        r config set ext-storage-admission-policy dram
        r config set ext-storage-promotion-policy always

        # Write a key with a value large enough to be spillable
        set padding [string repeat "P" 512]
        r set promoalways:k1 "promoval_${padding}"

        # Force spill via DEBUG SPILL
        r debug spill promoalways:k1
        wait_for_spill_complete promoalways:k1

        set on_flash_before [get_info_field "num_items_on_flash"]
        assert {$on_flash_before >= 1}

        # Fetch the key — should promote (bring back to DRAM)
        set val [r get promoalways:k1]
        assert {[string match "promoval_*" $val]}

        # After promotion, num_items_on_flash should decrement
        # (the key is now back in DRAM and no longer on flash)
        after 200
        set on_flash_after [get_info_field "num_items_on_flash"]
        assert {$on_flash_after < $on_flash_before}
    }

    # ========================================================================
    # Section 5: Promotion never — value served but stays on disk
    #
    # NOTE: The total_transient_promotions and permanent_promotions INFO
    # counters do NOT exist yet in ext_storage.c. They are planned for Phase
    # 2B (engine subagent). The assertions below that reference these counters
    # are commented out and marked GATED. Enable them once the engine work
    # lands and the counters are wired into genExternalStorageInfoString.
    # ========================================================================

    test {Promotion never: fetched value is correct but key stays on flash} {
        r flushall
        r config set ext-storage-admission-policy dram
        r config set ext-storage-promotion-policy never

        # Write and spill a key
        set padding [string repeat "N" 512]
        r set promonever:k1 "neverval_${padding}"
        r debug spill promonever:k1
        wait_for_spill_complete promonever:k1

        set on_flash_before [get_info_field "num_items_on_flash"]
        assert {$on_flash_before >= 1}

        # Fetch the key — value should be returned correctly
        set val [r get promonever:k1]
        assert {[string match "neverval_*" $val]}

        # With promotion=never, key should REMAIN on flash after fetch.
        # Allow time for async completion.
        after 200
        set on_flash_after [get_info_field "num_items_on_flash"]
        assert_equal $on_flash_after $on_flash_before

        # GATED on engine counters (Phase 2B):
        # Uncomment once total_transient_promotions is wired into INFO:
        # set transient [get_info_field "total_transient_promotions"]
        # assert {$transient >= 1}
        # set permanent [get_info_field "permanent_promotions"]
        # assert {$permanent == 0}
    }

    test {Promotion never: repeated fetches serve correct value each time} {
        r flushall
        r config set ext-storage-admission-policy dram
        r config set ext-storage-promotion-policy never

        set padding [string repeat "R" 512]
        r set promonever:k2 "repeatval_${padding}"
        r debug spill promonever:k2
        wait_for_spill_complete promonever:k2

        # Multiple GETs should all return the correct value
        for {set i 0} {$i < 5} {incr i} {
            set val [r get promonever:k2]
            assert {[string match "repeatval_*" $val]}
            after 50
        }

        # Key should still be on flash after all fetches
        set on_flash [get_info_field "num_items_on_flash"]
        assert {$on_flash >= 1}
    }

    # ========================================================================
    # Section 6: Embedded-value floor — sub-floor value stays resident
    # ========================================================================

    test {Admission flash: sub-floor embedded value stays resident (no crash)} {
        r flushall
        r config set ext-storage-admission-policy flash

        # Use a very small value that falls below the embstr/spill floor (~128 bytes)
        r set emb:small "tiny"

        # Should not crash, value should be correct
        set val [r get emb:small]
        assert_equal $val "tiny"

        # flash_admit_skipped_floor should have incremented (value too small)
        set skipped [get_info_field "flash_admit_skipped_floor"]
        assert {$skipped >= 1}
    }

    test {Admission flash: sub-floor value still works with GET/SET} {
        r config set ext-storage-admission-policy flash

        # Multiple small values — all should stay resident, no crash
        for {set i 0} {$i < 20} {incr i} {
            r set "emb:k$i" "v$i"
        }

        for {set i 0} {$i < 20} {incr i} {
            set val [r get "emb:k$i"]
            assert_equal $val "v$i"
        }
    }

    # ========================================================================
    # Section 7: Policy combination — flash + never
    # ========================================================================

    test {Flash admission + promotion never: full cycle} {
        r flushall
        r config set ext-storage-admission-policy flash
        r config set ext-storage-promotion-policy never

        set flash_before [get_info_field "flash_admit_writes"]

        # Write a key — should be flash-admitted
        set padding [string repeat "C" 512]
        r set combo:k1 "comboval_${padding}"

        wait_for_flash_admit [expr {$flash_before + 1}]

        # GET should return correct value
        set val [r get combo:k1]
        assert {[string match "comboval_*" $val]}

        # After fetch with promotion=never, key should stay on flash
        after 200
        set on_flash [get_info_field "num_items_on_flash"]
        assert {$on_flash >= 1}
    }

    # ========================================================================
    # Section 8: Runtime policy switch
    # ========================================================================

    test {Runtime switch from admission=dram to flash takes effect immediately} {
        r flushall
        r config set ext-storage-admission-policy dram

        set padding [string repeat "S" 256]
        r set switch:k1 "before_${padding}"

        # No flash admits yet
        set flash_before [get_info_field "flash_admit_writes"]

        # Switch to flash admission
        r config set ext-storage-admission-policy flash

        r set switch:k2 "after_${padding}"
        wait_for_flash_admit [expr {$flash_before + 1}]

        set flash_after [get_info_field "flash_admit_writes"]
        assert {$flash_after > $flash_before}

        # Both keys should be readable
        assert {[string match "before_*" [r get switch:k1]]}
        assert {[string match "after_*" [r get switch:k2]]}
    }
}