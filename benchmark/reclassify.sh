#!/usr/bin/env bash
# reclassify.sh — re-derive PASS/FAIL verdicts from saved audit logs without re-running.
# Usage: ./reclassify.sh results/<tag>
set -uo pipefail
D="${1:?usage: reclassify.sh results/<tag>}"
printf '%-16s %-26s %-8s %s\n' SCENARIO CONFIG VERDICT DETAIL
echo "scenario,config,verdict,detail" > "$D/audit-reclassified.csv"

for LOG in "$D"/*--*.log; do
    [ -f "$LOG" ] || continue
    BASE="$(basename "$LOG" .log)"
    SCEN="${BASE%%--*}"
    CFG="${BASE#*--}"

    if grep -q 'command not found' "$LOG"; then
        VERDICT=FAIL; DETAIL="config parse error: $(grep -m1 'command not found' "$LOG" | sed 's/.*line/line/')"
    elif grep -q 'Server failed to start' "$LOG"; then
        VERDICT=FAIL; DETAIL="server failed to start"
    elif grep -q 'Config not found' "$LOG"; then
        VERDICT=FAIL; DETAIL="config not found on remote"
    elif grep -q 'POPULATE_ABORTED' "$LOG"; then
        VERDICT=FAIL; DETAIL="$(grep -m1 -o 'POPULATE_ABORTED.*' "$LOG")"
    elif grep -qE 'ERROR:|error: ' "$LOG"; then
        VERDICT=FAIL; DETAIL="$(grep -m1 -E 'ERROR:|error: ' "$LOG" | cut -c1-110)"
    else
        THRU="$(grep -hoE '^"(GET|SET)","[0-9.]+"|[0-9.]+ requests per second|[0-9.]+ rps|Throughput: +[0-9]+ ops/s' "$LOG" | head -3 | paste -sd' ')"
        RETRIES=$(grep -c 'Populate attempt' "$LOG")
        if [ -n "$THRU" ]; then VERDICT=PASS; DETAIL="$THRU"
        elif [ "$RETRIES" -ge 5 ]; then
            VERDICT=GRIND
            DETAIL="populate never converged: $RETRIES retries, $(grep -o 'DBSIZE=[0-9]*/[0-9]*' "$LOG" | tail -1)"
        elif ! grep -q 'All done' "$LOG"; then VERDICT=TIMEOUT; DETAIL="run did not reach completion"
        else VERDICT=FAIL; DETAIL="no throughput line in output"; fi
    fi

    printf '%-16s %-26s %-8s %s\n' "$SCEN" "$CFG" "$VERDICT" "$(echo "$DETAIL" | cut -c1-80)"
    printf '%s,%s,%s,"%s"\n' "$SCEN" "$CFG" "$VERDICT" "${DETAIL//\"/\'}" >> "$D/audit-reclassified.csv"
done

echo ""
awk -F, 'NR>1{c[$3]++} END{for(v in c) printf "  %-8s %d\n", v, c[v]}' "$D/audit-reclassified.csv"
