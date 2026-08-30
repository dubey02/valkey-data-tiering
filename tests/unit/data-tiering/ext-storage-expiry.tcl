# Data tiering + expiry integration tests.
# Uses real FlashCache backend (requires fallocate).
#
# Run with:
#   ./runtest --single unit/data-tiering/ext-storage-expiry

set flash_path "/tmp/fc-test-expiry-[pid].db"

# Pre-allocate FlashCache file
exec fallocate -l 256M $flash_path

proc wait_for_spill {expected_min {timeout 15000}} {
    set start [clock milliseconds]
    while {1} {
        set info [r info all]
        if {[regexp {total_num_items_spilled_to_ext_storage:(\d+)} $info _ spilled]} {
            if {$spilled >= $expected_min} { return $spilled }
        }
        if {[clock milliseconds] - $start > $timeout} {
            error "Timed out waiting for spill (got $spilled, wanted $expected_min)"
        }
        after 100
    }
}

proc fill_memory {} {
    set padding [string repeat "X" 1024]
    for {set i 0} {$i < 10000} {incr i} {
        catch {r set "filler:[format %05d $i]" "${padding}_$i"}
    }
}

start_server [list tags {"ext-storage-expiry"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend flashcache \
    ext-storage-path $flash_path \
    ext-storage-capacity-mb 256 \
    maxmemory 8mb \
    maxmemory-policy allkeys-lru \
    hz 100 \
]] {

    test {Expiry: TTL on tiered key returns correct value} {
        r set ttlkey "value-for-ttl-test-padded-1234567890" EX 60
        fill_memory
        wait_for_spill 50
        set ttl [r ttl ttlkey]
        assert {$ttl > 0 && $ttl <= 60}
    }

    test {Expiry: PTTL on tiered key} {
        set pttl [r pttl ttlkey]
        assert {$pttl > 0 && $pttl <= 60000}
    }

    test {Expiry: PERSIST removes TTL on tiered key} {
        r set persistkey "persist-value-padded-1234567890" EX 60
        after 200
        r persist persistkey
        assert_equal [r ttl persistkey] -1
    }

    test {Expiry: EXPIRE sets TTL on tiered key} {
        r set expkey "expire-value-padded-1234567890"
        after 200
        r expire expkey 30
        set ttl [r ttl expkey]
        assert {$ttl > 0 && $ttl <= 30}
    }

    test {Expiry: passive - GET on expired tiered key returns empty} {
        r flushdb
        after 1000
        r set shortlived "value-that-will-expire-on-flash-padding" EX 4
        fill_memory
        wait_for_spill 50
        assert_equal [r exists shortlived] 1
        after 4500
        assert_equal [r get shortlived] {}
    }

    test {Expiry: passive - EXISTS returns 0 for expired tiered key} {
        assert_equal [r exists shortlived] 0
    }

    test {Expiry: active - cron removes expired tiered keys} {
        r flushdb
        after 1000
        # Set TTL keys first, then fill memory to trigger spilling of these keys
        for {set i 0} {$i < 50} {incr i} {
            r set "ae:$i" "active-expiry-value-$i-padded-for-size" EX 4
        }
        fill_memory
        wait_for_spill 50
        set before [r dbsize]
        # The TTL is 4s and the active-expire cron sweeps in sampled batches, so
        # the drop is not instantaneous. Poll to a deadline rather than sleeping
        # a fixed 5s: on a loaded box that single sleep can land before the cron
        # has swept, reporting 0 expired, which reads as a product failure when
        # it is only a timing shortfall.
        set deadline [expr {[clock milliseconds] + 30000}]
        set expired 0
        while {[clock milliseconds] < $deadline} {
            set expired [expr {$before - [r dbsize]}]
            if {$expired >= 45} break
            after 100
        }
        assert {$expired >= 45}
    }

    test {Expiry: server alive after expiry (no crash)} {
        assert_equal [r ping] PONG
    }

    test {Expiry: FLUSHDB clears tiered keys} {
        r flushdb
        after 1000
        fill_memory
        wait_for_spill 10
        assert {[r dbsize] > 0}
        r flushdb
        after 500
        assert_equal [r dbsize] 0
    }

    test {Expiry: FLUSHALL clears tiered keys} {
        r flushdb
        after 1000
        fill_memory
        wait_for_spill 10
        assert {[r dbsize] > 0}
        r flushall
        after 500
        assert_equal [r dbsize] 0
        assert_equal [r ping] PONG
    }

    test {Expiry: key expires during fetch (no wasted promotion)} {
        r flushdb
        after 1000
        # Seed WITHOUT a short TTL. The original set EX 2 here, then ran
        # fill_memory + wait_for_spill (up to 15s under load) before asserting
        # the key still existed — so the key routinely expired during its own
        # setup and the assertion below failed with exists==0. Arm the fuse only
        # after the key is confirmed resident on flash.
        r set fetchexpire "value-that-will-expire-during-fetch-padding"
        fill_memory
        wait_for_spill 50
        # Key should exist (not yet expired)
        assert_equal [r exists fetchexpire] 1

        # Arm a short fuse now. Guard that this did not promote the value back
        # into memory — if it had, the test would silently stop covering the
        # expire-while-on-flash path it exists to check.
        #
        # The guard is one-sided on purpose. num_items_on_flash is a live gauge
        # and fill_memory's spills are still completing asynchronously here, so
        # it drifts upward on its own; an exact equality check flakes by
        # construction (observed 5463 vs 5464). A promotion moves the gauge
        # DOWN, so "did not decrease" is the assertion that actually carries
        # weight. It cannot catch a promotion that a concurrent spill masks in
        # the same instant, which is an accepted limit rather than a hidden one.
        regexp {num_items_on_flash:(\d+)} [r info all] _ _on_flash_before
        r pexpire fetchexpire 200
        regexp {num_items_on_flash:(\d+)} [r info all] _ _on_flash_after
        assert {$_on_flash_after >= $_on_flash_before}

        after 1000
        # GET triggers fetch from flash, but key is expired — should return nil
        assert_equal [r get fetchexpire] {}
        # Key should be fully gone
        assert_equal [r exists fetchexpire] 0
    }

    test {Expiry: server alive after all tests (no crash)} {
        assert_equal [r ping] PONG
    }
}

# Cleanup
catch {file delete $flash_path}
