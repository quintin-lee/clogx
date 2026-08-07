#!/usr/bin/env bash
# Benchmark regression guard.
#
# Runs both benchmark binaries, extracts the key throughput metrics, and either
# records a fresh baseline or compares the current run against an existing one.
# Any metric that has regressed by more than the threshold (default 30%)
# relative to the baseline causes a non-zero exit (and thus a CI failure).
#
# Metrics tracked (all in msgs/sec):
#   throughput   - benchmark_throughput
#   sync_msgs    - benchmark_async_vs_sync, sync mode
#   async_msgs   - benchmark_async_vs_sync, async mode
#
# Usage:
#   scripts/benchmark_check.sh \
#       --results <file>       # write current-run metrics (required)
#       --baseline <file>      # baseline file to compare against (optional)
#       --samples <n>          # runs per metric when updating baseline, default 3
#       --threshold <percent>  # allowed regression %, default 30
#       --update-baseline      # record median of samples as the new baseline
#
# If --baseline is absent (file missing) and --update-baseline is not given,
# the current run is recorded as baseline (first-run bootstrap) and exit is 0.
# Run from the repo root.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

RESULTS=""
BASELINE=""
SAMPLES=3
THRESHOLD=30
UPDATE_BASELINE=0

usage() {
    sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --results)   RESULTS="$2"; shift 2 ;;
        --baseline)  BASELINE="$2"; shift 2 ;;
        --samples)   SAMPLES="$2"; shift 2 ;;
        --threshold) THRESHOLD="$2"; shift 2 ;;
        --update-baseline) UPDATE_BASELINE=1; shift ;;
        *) usage ;;
    esac
done

[[ -n "$RESULTS" ]] || { echo "error: --results is required" >&2; usage; }

require_bin() {
    [[ -x "$1" ]] || { echo "error: benchmark binary not found: $1 (run 'make benchmark' first)" >&2; exit 1; }
}
require_bin ./build/benchmark_throughput
require_bin ./build/benchmark_async_vs_sync

# ---- 1. Collect samples ---------------------------------------------------
# Each sample produces three metrics. For --update-baseline we gather SAMPLES
# of each and take the median (robust to transient machine noise).
measure_once() {
    local t s a
    t="$(./build/benchmark_throughput 2>&1 | sed -n 's/^throughput: \([0-9][0-9]*\) msgs\/sec.*/\1/p' | tail -n1)"
    local out
    out="$(./build/benchmark_async_vs_sync 2>&1)"
    s="$(printf '%s\n' "$out" | sed -n 's/^mode=sync[[:space:]].*msgs\/sec=\([0-9][0-9]*\).*/\1/p' | tail -n1)"
    a="$(printf '%s\n' "$out" | sed -n 's/^mode=async[[:space:]].*msgs\/sec=\([0-9][0-9]*\).*/\1/p' | tail -n1)"
    for v in "$t" "$s" "$a"; do
        [[ -n "$v" ]] || { echo "error: could not parse benchmark metrics" >&2; exit 1; }
    done
    printf '%s\n%s\n%s\n' "$t" "$s" "$a"
}

median() {
    # median of newline-separated integers on stdin
    sort -n | awk '{a[NR]=$1} END{ n=NR; if(n%2==1) print a[(n+1)/2]; else print int((a[n/2]+a[n/2+1])/2) }'
}

if (( UPDATE_BASELINE )); then
    # Gather samples per metric, reduce to median.
    t_list=""; s_list=""; a_list=""
    for _ in $(seq 1 "$SAMPLES"); do
        mapfile -t m < <(measure_once)
        tm="${m[0]}"; sm="${m[1]}"; am="${m[2]}"
        t_list+="$tm
"; s_list+="$sm
"; a_list+="$am
"
    done
    T="$(printf '%s' "$t_list" | median)"
    S="$(printf '%s' "$s_list" | median)"
    A="$(printf '%s' "$a_list" | median)"
else
    mapfile -t m < <(measure_once)
    T="${m[0]}"; S="${m[1]}"; A="${m[2]}"
fi

cat > "$RESULTS" <<EOF
throughput_throughput=$T
sync_sync=$S
async_async=$A
EOF
cat "$RESULTS"

# ---- 2. Update baseline (skip comparison) --------------------------------
if [[ "$UPDATE_BASELINE" -eq 1 || -z "$BASELINE" || ! -f "$BASELINE" ]]; then
    if [[ -n "$BASELINE" ]]; then
        cp "$RESULTS" "$BASELINE"
        echo "benchmark_check: baseline recorded -> $BASELINE (median of $SAMPLES samples, skip comparison)"
    else
        echo "benchmark_check: no baseline, current run recorded as baseline (skip comparison)"
    fi
    exit 0
fi

# ---- 3. Compare against baseline -----------------------------------------
failed=0
while IFS='=' read -r metric value; do
    [[ -n "$metric" ]] || continue
    base="$(sed -n "s/^${metric}=//p" "$BASELINE" | tail -n1)"
    [[ "$base" =~ ^[0-9]+$ ]] || continue
    min="$(awk -v b="$base" -v t="$THRESHOLD" 'BEGIN{printf "%d", b - int(b*t/100)}')"
    if (( value < min )); then
        echo "FAIL: $metric regressed $base -> $value (min allowed $min, $THRESHOLD%)"
        failed=1
    else
        echo "ok:   $metric=$value (baseline $base, min $min)"
    fi
done < "$RESULTS"

if (( failed )); then
    echo "==benchmark_check: FAILED - performance regression detected"
    exit 1
fi
echo "==benchmark_check: PASS - no regression beyond ${THRESHOLD}%"
exit 0
