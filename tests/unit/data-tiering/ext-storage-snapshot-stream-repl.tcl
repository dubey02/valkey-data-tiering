#
# Streaming snapshot: replication integration (Phase 3)
#
# rdbSaveToReplicasSockets had no tiering handling at all: a diskless full sync
# with flash-resident values ran the child's preads with the storage IO thread
# unparked and on-flash GC unpaused, so the offsets it read were not guaranteed
# stable. This suite covers that path now that it takes the same cut-then-fork
# as rdbSaveBackground, plus the fork read quiesce as its fallback.
#
# Three transports reach a replica and each forks separately:
#   - diskless, piped through the parent      (rioInitWithFd)
#   - diskless dual channel, straight to conn (rioInitWithConnset)
#   - disk backed                             (rdbSaveBackground)
#
# Every test asserts on the flash section log line from the PRIMARY's child, so
# a silent fall back to the fork read path cannot pass vacuously.
#
# Runs against the mock by default. For real FlashCache:
#   EXT_STORAGE_BACKEND=flashcache ./runtest --single unit/data-tiering/ext-storage-snapshot-stream-repl

proc repl_counter {srv name} {
    set info [$srv info all]
    if {[regexp "${name}:(\\d+)" $info _ val]} { return $val }
    return 0
}

proc repl_spill_wait {srv key {timeout 5000}} {
    set before [repl_counter $srv total_num_items_spilled_to_ext_storage]
    $srv debug spill $key
    set start [clock milliseconds]
    while {1} {
        if {[repl_counter $srv total_num_items_spilled_to_ext_storage] > $before} { return }
        if {[clock milliseconds] - $start > $timeout} { error "Timed out spilling '$key'" }
        after 50
    }
}

proc repl_incompressible {len seed} {
    set out ""
    set x $seed
    while {[string length $out] < $len} {
        set x [expr {($x * 1103515245 + 12345) & 0x7fffffff}]
        append out [format %08x $x]
    }
    return [string range $out 0 [expr {$len - 1}]]
}

# Entries the flash section reported in $logfile after line $from, -1 if it did
# not run at all.
proc repl_flash_entries {logfile from} {
    set fd [open $logfile r]
    set lines [split [read $fd] "\n"]
    close $fd
    set entries -1
    set i 0
    foreach line $lines {
        incr i
        if {$i <= $from} continue
        if {[regexp {flash section: (\d+) entries written} $line _ n]} { set entries $n }
        if {[string match {*falling back to the fork read path*} $line]} { set entries -1 }
    }
    return $entries
}

proc repl_log_lines {logfile} {
    set fd [open $logfile r]
    set n [llength [split [read $fd] "\n"]]
    close $fd
    return $n
}

if {[info exists ::env(EXT_STORAGE_BACKEND)]} {
    set _backend $::env(EXT_STORAGE_BACKEND)
} else {
    set _backend "flashcache-mock"
}
set _ppath "/tmp/fc-test-snaprepl-p-[pid].db"
set _rpath "/tmp/fc-test-snaprepl-r-[pid].db"
catch {exec fallocate -l 256M $_ppath}
catch {exec fallocate -l 256M $_rpath}

# Replica first so it is server -1 relative to the primary's scope.
start_server [list tags {"ext-storage" "ext-storage-snapshot-stream-repl"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_rpath \
    ext-storage-capacity-mb 256 \
    maxmemory 100mb \
    maxmemory-policy allkeys-lru \
    appendonly no \
    save {} \
]] {
start_server [list tags {"ext-storage" "ext-storage-snapshot-stream-repl"} overrides [list \
    ext-storage-enabled yes \
    ext-storage-backend $_backend \
    ext-storage-path $_ppath \
    ext-storage-capacity-mb 256 \
    maxmemory 100mb \
    maxmemory-policy allkeys-lru \
    appendonly no \
    save {} \
    repl-diskless-sync yes \
    repl-diskless-sync-delay 0 \
]] {
    set primary       [srv 0 client]
    set primary_host  [srv 0 host]
    set primary_port  [srv 0 port]
    set primary_log   [srv 0 stdout]
    set replica       [srv -1 client]

    # Seed the primary once and reuse it: each test detaches the replica, forces
    # a fresh full sync, and checks what arrived.
    proc seed_primary {p n} {
        $p flushall
        for {set i 0} {$i < $n} {incr i} {
            $p set rk:$i [repl_incompressible [expr {300 + $i}] [expr {$i * 19 + 7}]]
            repl_spill_wait $p rk:$i
        }
        # A few memory-resident keys so the memory section is non-empty too.
        for {set i 0} {$i < 20} {incr i} { $p set rmem:$i "rm$i" }
    }

    proc verify_replica {rep n} {
        assert_equal [expr {$n + 20}] [$rep dbsize]
        for {set i 0} {$i < $n} {incr i} {
            assert_equal [repl_incompressible [expr {300 + $i}] [expr {$i * 19 + 7}]] [$rep get rk:$i]
        }
        for {set i 0} {$i < 20} {incr i} { assert_equal "rm$i" [$rep get rmem:$i] }
    }

    test {repl diskless: flash section streams to a replica through the parent pipe} {
        $replica replicaof no one
        $primary config set dual-channel-replication-enabled no
        set n 150
        seed_primary $primary $n

        set from [repl_log_lines $primary_log]
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        wait_for_ofs_sync $primary $replica

        assert_equal $n [repl_flash_entries $primary_log $from]
        verify_replica $replica $n
    }

    test {repl diskless dual channel: flash section streams straight to the connection} {
        $replica replicaof no one
        $primary config set dual-channel-replication-enabled yes
        $replica config set dual-channel-replication-enabled yes
        set n 120
        seed_primary $primary $n

        set from [repl_log_lines $primary_log]
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        wait_for_ofs_sync $primary $replica

        assert_equal $n [repl_flash_entries $primary_log $from]
        verify_replica $replica $n
        $primary config set dual-channel-replication-enabled no
        $replica config set dual-channel-replication-enabled no
    }

    test {repl disk backed: flash section streams via the BGSAVE child} {
        $replica replicaof no one
        $primary config set repl-diskless-sync no
        set n 100
        seed_primary $primary $n

        set from [repl_log_lines $primary_log]
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        wait_for_ofs_sync $primary $replica

        assert_equal $n [repl_flash_entries $primary_log $from]
        verify_replica $replica $n
        $primary config set repl-diskless-sync yes
    }

    test {repl diskless: deleted keys are not resurrected on the replica} {
        $replica replicaof no one
        set n 100
        seed_primary $primary $n
        for {set i 0} {$i < $n} {incr i 2} { $primary del rk:$i }

        set from [repl_log_lines $primary_log]
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        wait_for_ofs_sync $primary $replica

        assert_equal [expr {$n / 2}] [repl_flash_entries $primary_log $from]
        assert_equal [expr {$n / 2 + 20}] [$replica dbsize]
        for {set i 0} {$i < $n} {incr i} {
            assert_equal [expr {$i % 2 == 0 ? 0 : 1}] [$replica exists rk:$i]
        }
    }

    test {repl diskless: fork read fallback still syncs a replica} {
        $replica replicaof no one
        $primary config set ext-storage-snapshot-stream no
        set n 80
        seed_primary $primary $n

        set from [repl_log_lines $primary_log]
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        wait_for_ofs_sync $primary $replica

        # The override short circuits before the barrier, so neither the flash
        # section nor a fallback notice should appear.
        assert_equal -1 [repl_flash_entries $primary_log $from]
        verify_replica $replica $n
        $primary config set ext-storage-snapshot-stream yes
    }

    test {repl: writes after the cut reach the replica} {
        $replica replicaof no one
        set n 60
        seed_primary $primary $n
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica

        for {set i 0} {$i < 30} {incr i} { $primary set post:$i "after-cut-$i" }
        wait_for_ofs_sync $primary $replica
        for {set i 0} {$i < 30} {incr i} { assert_equal "after-cut-$i" [$replica get post:$i] }
    }

    test {repl: both servers healthy afterwards} {
        assert_equal "PONG" [$primary ping]
        assert_equal "PONG" [$replica ping]
    }
}
}

catch {file delete $_ppath}
catch {file delete $_rpath}
