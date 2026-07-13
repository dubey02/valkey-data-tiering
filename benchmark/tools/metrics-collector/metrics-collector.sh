#!/bin/bash
# metrics-collector.sh - Poll Valkey INFO + system stats to CSV
# Usage: metrics-collector.sh PORT OUTFILE [INTERVAL]
set -o pipefail

PORT="${1:?Usage: $0 PORT OUTFILE [INTERVAL]}"
OUTFILE="${2:?Usage: $0 PORT OUTFILE [INTERVAL]}"
INTERVAL="${3:-1}"
CLI="${VALKEY_CLI:-valkey-cli}"

HEADER="timestamp,used_memory,used_memory_rss,maxmemory,keyspace_hits,keyspace_misses,ops_per_sec,total_commands_delta,total_num_items_spilled_to_ext_storage,total_num_items_fetched_from_ext_storage,completion_read_ok,dram_value_hits,kbc_fetching_block,num_items_spilling_to_ext_storage,blocked_clients,flash_admit_writes,total_transient_promotions,transient_promotion_clients_served,permanent_promotions,cpu_user,cpu_sys,valkey_cpu_user,valkey_cpu_sys,valkey_cpu_total,asio_cpu_pct,disk_hit_pct,mem_hit_pct, mem_frag_ratio,disk_read_iops,disk_write_iops,disk_read_mb,disk_write_mb,disk_read_merges_ps,disk_write_merges_ps,disk_r_await_ms,disk_w_await_ms,disk_aqu_sz,disk_util_pct,disk_in_flight,disk_req_sz_kb,throttle_total_throttled,throttle_queued_clients,throttle_current_rate,throttle_allowed_tps,spill_submitted_count,spill_serialized_count,mean_spill_ram,inflight_spill_ram_bytes"
echo "$HEADER" > "$OUTFILE"

# Full raw INFO ALL snapshot per tick, appended to a sibling log so we can mine
# any field later even if it's not in the CSV header above. Delimited by timestamp.
# Uses a .log extension so the report's CSV glob never picks it up.
INFO_DUMP="$(dirname "$OUTFILE")/info-full.log"
: > "$INFO_DUMP"

get_info_field() { echo "$INFO" | grep -m1 "^${1}:" | cut -d: -f2 | tr -d '\r'; }

prev_cpu_user=0; prev_cpu_sys=0; prev_cpu_idle=0; prev_cpu_total=1
prev_valkey_cpu_user=0; prev_valkey_cpu_sys=0
read_cpu() {
    local line; line=$(head -1 /proc/stat)
    local u s n i; u=$(echo "$line"|awk '{print $2+$3}'); s=$(echo "$line"|awk '{print $4}')
    n=$(echo "$line"|awk '{print $5}'); i=$n
    local total=$((u+s+i))
    local du=$((u-prev_cpu_user)); local ds=$((s-prev_cpu_sys)); local dt=$((total-prev_cpu_total))
    [ "$dt" -eq 0 ] && dt=1
    cpu_user=$(awk "BEGIN{printf \"%.1f\", $du/$dt*100}")
    cpu_sys=$(awk "BEGIN{printf \"%.1f\", $ds/$dt*100}")
    prev_cpu_user=$u; prev_cpu_sys=$s; prev_cpu_total=$total
}

prev_rd_sectors=0; prev_wr_sectors=0; prev_rd_ios=0; prev_wr_ios=0; prev_disk_ts=0
prev_rd_merges=0; prev_wr_merges=0; prev_rd_ticks=0; prev_wr_ticks=0; prev_io_ticks=0; prev_tiq=0
read_disk() {
    disk_read_iops=0; disk_write_iops=0; disk_read_mb=0; disk_write_mb=0
    disk_read_merges_ps=0; disk_write_merges_ps=0; disk_r_await_ms=0; disk_w_await_ms=0
    disk_aqu_sz=0; disk_util_pct=0; disk_in_flight=0; disk_req_sz_kb=0
    local dev; dev=$(lsblk -dno NAME,MOUNTPOINT 2>/dev/null | awk '$2=="/mnt/nvme"{print $1}' || true)
    [ -z "$dev" ] && dev=$(lsblk -dno NAME,SIZE 2>/dev/null | grep 'nvme\|sd' | sort -k2 -h | tail -1 | awk '{print $1}' || true)
    [ -z "$dev" ] && return
    local stats; stats=$(cat "/sys/block/$dev/stat" 2>/dev/null) || return
    # /sys/block/<dev>/stat fields: 1 rd_ios 2 rd_merges 3 rd_sec 4 rd_ticks(ms)
    #   5 wr_ios 6 wr_merges 7 wr_sec 8 wr_ticks(ms) 9 in_flight 10 io_ticks(ms) 11 time_in_queue(ms)
    local rd_ios wr_ios rd_sec wr_sec rd_merges wr_merges rd_ticks wr_ticks in_flight io_ticks tiq
    rd_ios=$(echo "$stats"|awk '{print $1}');  rd_merges=$(echo "$stats"|awk '{print $2}')
    rd_sec=$(echo "$stats"|awk '{print $3}');  rd_ticks=$(echo "$stats"|awk '{print $4}')
    wr_ios=$(echo "$stats"|awk '{print $5}');  wr_merges=$(echo "$stats"|awk '{print $6}')
    wr_sec=$(echo "$stats"|awk '{print $7}');  wr_ticks=$(echo "$stats"|awk '{print $8}')
    in_flight=$(echo "$stats"|awk '{print $9}'); io_ticks=$(echo "$stats"|awk '{print $10}')
    tiq=$(echo "$stats"|awk '{print $11}')
    disk_in_flight=${in_flight:-0}
    local now; now=$(date +%s)
    if [ "$prev_disk_ts" -gt 0 ]; then
        local dt=$((now-prev_disk_ts)); [ "$dt" -eq 0 ] && dt=1
        local drd=$((rd_ios-prev_rd_ios)); local dwr=$((wr_ios-prev_wr_ios))
        disk_read_iops=$(( drd/dt )); disk_write_iops=$(( dwr/dt ))
        disk_read_mb=$(awk "BEGIN{printf \"%.2f\", ($rd_sec-$prev_rd_sectors)*512/1048576/$dt}")
        disk_write_mb=$(awk "BEGIN{printf \"%.2f\", ($wr_sec-$prev_wr_sectors)*512/1048576/$dt}")
        disk_read_merges_ps=$(awk "BEGIN{printf \"%.1f\", ($rd_merges-$prev_rd_merges)/$dt}")
        disk_write_merges_ps=$(awk "BEGIN{printf \"%.1f\", ($wr_merges-$prev_wr_merges)/$dt}")
        # await = ticks delta / ios delta (ms per IO); aqu-sz = time_in_queue delta / interval(ms)
        disk_r_await_ms=$(awk "BEGIN{d=$drd; if(d>0) printf \"%.2f\", ($rd_ticks-$prev_rd_ticks)/d; else printf \"0.00\"}")
        disk_w_await_ms=$(awk "BEGIN{d=$dwr; if(d>0) printf \"%.2f\", ($wr_ticks-$prev_wr_ticks)/d; else printf \"0.00\"}")
        disk_aqu_sz=$(awk "BEGIN{printf \"%.2f\", ($tiq-$prev_tiq)/($dt*1000)}")
        # util% = io_ticks(ms) busy over interval(ms), capped at 100
        disk_util_pct=$(awk "BEGIN{u=($io_ticks-$prev_io_ticks)/($dt*1000)*100; if(u>100)u=100; printf \"%.1f\", u}")
        # avg request size (KB) over both r+w sectors this interval
        disk_req_sz_kb=$(awk "BEGIN{ios=$drd+$dwr; if(ios>0) printf \"%.1f\", (($rd_sec-$prev_rd_sectors)+($wr_sec-$prev_wr_sectors))*512/ios/1024; else printf \"0.0\"}")
    fi
    prev_rd_ios=$rd_ios; prev_wr_ios=$wr_ios
    prev_rd_sectors=$rd_sec; prev_wr_sectors=$wr_sec
    prev_rd_merges=$rd_merges; prev_wr_merges=$wr_merges
    prev_rd_ticks=$rd_ticks; prev_wr_ticks=$wr_ticks
    prev_io_ticks=$io_ticks; prev_tiq=$tiq; prev_disk_ts=$now
}

# ASIO/FC IO thread CPU: find thread named 'fc_io_worker' via /proc/PID/task/TID/comm
prev_asio_ticks=0; prev_asio_ts=0
asio_cpu_pct=0
read_asio_cpu() {
    asio_cpu_pct=0
    local pid; pid=$(pgrep -x valkey-server 2>/dev/null | head -1)
    [ -z "$pid" ] && return
    local hz=100
    local asio_ticks=0
    for tdir in /proc/$pid/task/*/; do
        local comm; comm=$(cat "${tdir}comm" 2>/dev/null) || continue
        [ "$comm" != "fc_io_worker" ] && continue
        local stat; stat=$(cat "${tdir}stat" 2>/dev/null) || continue
        local utime stime
        utime=$(echo "$stat" | sed 's/.*) //' | awk '{print $12}')
        stime=$(echo "$stat" | sed 's/.*) //' | awk '{print $13}')
        asio_ticks=$(( asio_ticks + utime + stime ))
    done
    local now; now=$(date +%s)
    if [ "$prev_asio_ts" -gt 0 ]; then
        local dt=$(( now - prev_asio_ts )); [ "$dt" -eq 0 ] && dt=1
        local dticks=$(( asio_ticks - prev_asio_ticks ))
        asio_cpu_pct=$(awk "BEGIN{printf \"%.1f\", $dticks / ($dt * $hz) * 100}")
    fi
    prev_asio_ticks=$asio_ticks; prev_asio_ts=$now
}

trap 'echo "metrics-collector: stopped after $(wc -l < "$OUTFILE") samples"; exit 0' INT TERM

prev_total_commands=0
while true; do
    INFO=$($CLI -p "$PORT" INFO ALL 2>/dev/null) || { sleep "$INTERVAL"; continue; }
    read_cpu; read_disk; read_asio_cpu
    # Per-process CPU (from INFO, cumulative seconds -> % per interval)
    vcu=$(get_info_field used_cpu_user_main_thread)
    vcs=$(get_info_field used_cpu_sys_main_thread)
    if [[ -n "$vcu" && -n "$vcs" ]]; then
        valkey_cpu_user=$(awk "BEGIN{printf \"%.1f\", ($vcu-$prev_valkey_cpu_user)*100}")
        valkey_cpu_sys=$(awk "BEGIN{printf \"%.1f\", ($vcs-$prev_valkey_cpu_sys)*100}")
        valkey_cpu_total=$(awk "BEGIN{printf \"%.1f\", $valkey_cpu_user+$valkey_cpu_sys}")
        prev_valkey_cpu_user=$vcu; prev_valkey_cpu_sys=$vcs
    else
        valkey_cpu_user=0; valkey_cpu_sys=0; valkey_cpu_total=0
    fi
    ts=$(date +%s)
    used_memory=$(get_info_field used_memory)
    used_memory_rss=$(get_info_field used_memory_rss)
    maxmemory=$(get_info_field maxmemory)
    keyspace_hits=$(get_info_field keyspace_hits)
    keyspace_misses=$(get_info_field keyspace_misses)
    ops_per_sec=$(get_info_field instantaneous_ops_per_sec); ops_per_sec=${ops_per_sec:-0}
    total_commands=$(get_info_field total_commands_processed); total_commands=${total_commands:-0}
    if [ "$prev_total_commands" -gt 0 ]; then
        total_commands_delta=$(( (total_commands - prev_total_commands) / INTERVAL ))
    else
        total_commands_delta=$ops_per_sec
    fi
    prev_total_commands=$total_commands
    total_num_items_spilled_to_ext_storage=$(get_info_field total_num_items_spilled_to_ext_storage); total_num_items_spilled_to_ext_storage=${total_num_items_spilled_to_ext_storage:-0}
    total_num_items_fetched_from_ext_storage=$(get_info_field total_num_items_fetched_from_ext_storage); total_num_items_fetched_from_ext_storage=${total_num_items_fetched_from_ext_storage:-0}
    completion_read_ok=$(get_info_field completion_read_ok); completion_read_ok=${completion_read_ok:-0}
    dram_value_hits=$(get_info_field dram_value_hits); dram_value_hits=${dram_value_hits:-0}
    kbc_fetching_block=$(get_info_field kbc_fetching_block); kbc_fetching_block=${kbc_fetching_block:-0}
    num_items_spilling_to_ext_storage=$(get_info_field num_items_spilling_to_ext_storage); num_items_spilling_to_ext_storage=${num_items_spilling_to_ext_storage:-0}
    blocked_clients=$(get_info_field blocked_clients)
    flash_admit_writes=$(get_info_field flash_admit_writes); flash_admit_writes=${flash_admit_writes:-0}
    total_transient_promotions=$(get_info_field total_transient_promotions); total_transient_promotions=${total_transient_promotions:-0}
    transient_promotion_clients_served=$(get_info_field transient_promotion_clients_served); transient_promotion_clients_served=${transient_promotion_clients_served:-0}
    permanent_promotions=$(get_info_field permanent_promotions); permanent_promotions=${permanent_promotions:-0}
    throttle_total_throttled=$(get_info_field throttle_total_throttled); throttle_total_throttled=${throttle_total_throttled:-0}
    throttle_queued_clients=$(get_info_field throttle_queued_clients); throttle_queued_clients=${throttle_queued_clients:-0}
    throttle_current_rate=$(get_info_field throttle_current_rate); throttle_current_rate=${throttle_current_rate:-0}
    throttle_allowed_tps=$(get_info_field throttle_allowed_tps); throttle_allowed_tps=${throttle_allowed_tps:-0}
    spill_submitted_count=$(get_info_field spill_submitted_count); spill_submitted_count=${spill_submitted_count:-0}
    spill_serialized_count=$(get_info_field spill_serialized_count); spill_serialized_count=${spill_serialized_count:-0}
    mean_spill_ram=$(get_info_field mean_spill_ram); mean_spill_ram=${mean_spill_ram:-0}
    inflight_spill_ram_bytes=$(get_info_field inflight_spill_ram_bytes); inflight_spill_ram_bytes=${inflight_spill_ram_bytes:-0}
    # Derived: disk hit % and mem hit % (engine formula: denominator = dram_value_hits
    # which includes both immediate DRAM hits AND re-execs after fetch completion).
    # disk_hit_pct = completion_read_ok / dram_value_hits (% that touched disk)
    # mem_hit_pct  = (dram_value_hits - completion_read_ok) / dram_value_hits (% served from DRAM only)
    # These two sum to 100% among value-accessing commands.
    if [ "${dram_value_hits:-0}" -gt 0 ]; then
        disk_hit_pct=$(awk "BEGIN{printf \"%.1f\", ${completion_read_ok:-0}/${dram_value_hits}*100}")
        mem_hit_pct=$(awk "BEGIN{printf \"%.1f\", (${dram_value_hits}-${completion_read_ok:-0})/${dram_value_hits}*100}")
    else
        disk_hit_pct="0.0"
        mem_hit_pct="0.0"
    fi
    mem_frag_ratio=$(get_info_field mem_fragmentation_ratio); mem_frag_ratio=${mem_frag_ratio:-0}
    echo "${ts},${used_memory},${used_memory_rss},${maxmemory},${keyspace_hits},${keyspace_misses},${ops_per_sec},${total_commands_delta},${total_num_items_spilled_to_ext_storage},${total_num_items_fetched_from_ext_storage},${completion_read_ok},${dram_value_hits},${kbc_fetching_block},${num_items_spilling_to_ext_storage},${blocked_clients},${flash_admit_writes},${total_transient_promotions},${transient_promotion_clients_served},${permanent_promotions},${cpu_user},${cpu_sys},${valkey_cpu_user},${valkey_cpu_sys},${valkey_cpu_total},${asio_cpu_pct},${disk_hit_pct},${mem_hit_pct},${mem_frag_ratio},${disk_read_iops},${disk_write_iops},${disk_read_mb},${disk_write_mb},${disk_read_merges_ps},${disk_write_merges_ps},${disk_r_await_ms},${disk_w_await_ms},${disk_aqu_sz},${disk_util_pct},${disk_in_flight},${disk_req_sz_kb},${throttle_total_throttled},${throttle_queued_clients},${throttle_current_rate},${throttle_allowed_tps},${spill_submitted_count},${spill_serialized_count},${mean_spill_ram},${inflight_spill_ram_bytes}" >> "$OUTFILE"
    # Append the full raw INFO ALL snapshot for this tick (timestamp-delimited).
    printf '===== INFO ALL @ %s =====\n%s\n\n' "$ts" "$INFO" >> "$INFO_DUMP"
    sleep "$INTERVAL"
done
