#!/usr/bin/env bash
# ANTS-5133 — run every perf benchmark and report it against a saved baseline.
#
# Usage:
#   tools/perf-report.sh                 # run, compare against the baseline
#   tools/perf-report.sh --save-baseline # run, then record these numbers
#   tools/perf-report.sh --json          # machine-readable, for a later tool
#   tools/perf-report.sh -R <regex>      # only benchmarks matching
#
# Options:
#   --build-dir DIR   tree to run from (default: build)
#   --threshold PCT   regression threshold, percent (default: 5)
#   --repeat N        run each benchmark N times, keep the best (default: 1)
#   --baseline FILE   baseline path (default: tests/perf/baseline.tsv)
#
# Exit codes: 0 all within threshold; 1 a regression; 2 a setup problem.
#
# HOW IT FINDS WORK. Benchmarks are discovered from ctest's `perf` label, and
# their numbers are read from the uniform ANTSPERF lines tests/perf/perf_metric.h
# emits. Neither the list of benchmarks nor the list of metrics is written down
# here. That is deliberate: a runner carrying its own copy of either is a
# hand-maintained parallel description of the suite, and ANTS-4392 records what
# that costs — the two drift, and the drift is invisible precisely when
# something that should have been measured was not.

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 2
ROOT="$PWD"

BUILD_DIR="build"
BASELINE="tests/perf/baseline.tsv"
THRESHOLD=5
REPEAT=1
SAVE=0
JSON=0
FILTER=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --save-baseline) SAVE=1; shift ;;
        --json)          JSON=1; shift ;;
        --build-dir)     BUILD_DIR="$2"; shift 2 ;;
        --baseline)      BASELINE="$2"; shift 2 ;;
        --threshold)     THRESHOLD="$2"; shift 2 ;;
        --repeat)        REPEAT="$2"; shift 2 ;;
        -R|--filter)     FILTER="$2"; shift 2 ;;
        -h|--help)       sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "perf-report: unknown argument $1" >&2; exit 2 ;;
    esac
done

[[ -d "$BUILD_DIR" ]] || { echo "perf-report: no build dir '$BUILD_DIR'" >&2; exit 2; }

# ── machine fingerprint ──────────────────────────────────────────────────────
# Recorded with the baseline and checked against it. These are wall-clock
# measurements on a shared desktop: a percentage computed across two different
# machines is not a comparison, and printing one anyway is worse than printing
# nothing, so a mismatch downgrades the run to "recorded, not compared".
machine_id() {
    local cpu cores mem
    cpu=$(awk -F': ' '/^model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null)
    cores=$(nproc 2>/dev/null)
    mem=$(awk '/^MemTotal/{printf "%.0fGiB", $2/1048576}' /proc/meminfo 2>/dev/null)
    printf '%s | %s cores | %s' "${cpu:-unknown}" "${cores:-?}" "${mem:-?}"
}
MACHINE="$(machine_id)"

# ── discover the benchmarks ──────────────────────────────────────────────────
# From the SOURCE tree, not from ctest and not from a list kept here. The
# sources are what exists; a list in this file would drift from them, and ctest
# -N triggers GoogleTest discovery, which runs every test binary in the tree
# just to enumerate names.
#
# A source with no built executable is REPORTED, not skipped quietly: "you did
# not build it" and "it emitted nothing" are different problems with different
# fixes, and a silent skip is how a metric goes missing unnoticed.
mapfile -t TESTS < <(find tests/perf -maxdepth 1 -name 'bench_*.cpp' -printf '%f\n' \
                     | sed 's/^bench_//; s/\.cpp$//' | sort)
if [[ ${#TESTS[@]} -eq 0 ]]; then
    echo "perf-report: no tests/perf/bench_*.cpp sources found." >&2
    exit 2
fi

declare -A VALUE UNIT DIR
ran=0 metric_count=0 skipped_count=0
skipped=()

for t in "${TESTS[@]}"; do
    [[ -n "$FILTER" && ! "$t" =~ $FILTER ]] && continue
    exe="$BUILD_DIR/bench_$t"
    if [[ ! -x "$exe" ]]; then
        skipped+=("bench_$t — not built (cmake --build $BUILD_DIR --target bench_$t)")
        ((skipped_count++))
        continue
    fi
    for ((i = 0; i < REPEAT; i++)); do
        while IFS=$'\t' read -r tag name val unit dir; do
            [[ "$tag" == "ANTSPERF" ]] || continue
            # Keep the best of REPEAT runs. A benchmark's worst runs are the
            # ones the desktop interfered with, and the best is the closest
            # reading of the code's own cost.
            if [[ -n "${VALUE[$name]:-}" ]]; then
                if [[ "$dir" == "lower_is_better" ]]; then
                    better=$(awk -v a="$val" -v b="${VALUE[$name]}" 'BEGIN{print (a<b)?1:0}')
                else
                    better=$(awk -v a="$val" -v b="${VALUE[$name]}" 'BEGIN{print (a>b)?1:0}')
                fi
                [[ "$better" == "1" ]] || continue
            fi
            [[ -n "${VALUE[$name]:-}" ]] || ((metric_count++))
            VALUE[$name]="$val"; UNIT[$name]="$unit"; DIR[$name]="$dir"
        done < <(QT_QPA_PLATFORM=offscreen "$exe" 2>/dev/null)
    done
    ((ran++))
done

if [[ "$metric_count" -eq 0 ]]; then
    echo "perf-report: ran $ran benchmark(s) and none emitted a metric line." >&2
    echo "  A benchmark reports through AntsPerf::report*Better (tests/perf/perf_metric.h)." >&2
    exit 2
fi

# ── save ─────────────────────────────────────────────────────────────────────
if [[ "$SAVE" == 1 ]]; then
    {
        # printf, not echo: echo does not interpret \t, and these header lines
        # are read back as tab-separated fields.
        echo "# ANTS-5133 perf baseline — regenerate with tools/perf-report.sh --save-baseline"
        printf '# date\t%s\n'    "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf '# machine\t%s\n' "$MACHINE"
        printf '# commit\t%s\n'  "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
        for n in $(printf '%s\n' "${!VALUE[@]}" | sort); do
            printf '%s\t%s\t%s\t%s\n' "$n" "${VALUE[$n]}" "${UNIT[$n]}" "${DIR[$n]}"
        done
    } > "$BASELINE"
    echo "perf-report: baseline written to $BASELINE ($metric_count metrics)"
fi

# ── load the baseline ────────────────────────────────────────────────────────
declare -A BASE
base_count=0
BASE_MACHINE="" BASE_DATE="" BASE_COMMIT=""
if [[ -f "$BASELINE" ]]; then
    while IFS=$'\t' read -r a b _c _d; do
        case "$a" in
            "# machine") BASE_MACHINE="$b"; continue ;;
            "# date")    BASE_DATE="$b";    continue ;;
            "# commit")  BASE_COMMIT="$b";  continue ;;
            \#*|"")      continue ;;
        esac
        BASE[$a]="$b"; ((base_count++))
    done < "$BASELINE"
fi

COMPARABLE=1
if [[ "$base_count" -eq 0 ]]; then
    COMPARABLE=0
elif [[ -n "$BASE_MACHINE" && "$BASE_MACHINE" != "$MACHINE" ]]; then
    COMPARABLE=2
fi

# ── JSON ─────────────────────────────────────────────────────────────────────
if [[ "$JSON" == 1 ]]; then
    printf '{\n  "machine": "%s",\n  "comparable": %s,\n  "metrics": {\n' \
           "$MACHINE" "$COMPARABLE"
    first=1
    for n in $(printf '%s\n' "${!VALUE[@]}" | sort); do
        [[ $first == 1 ]] || printf ',\n'; first=0
        printf '    "%s": {"value": %s, "unit": "%s", "direction": "%s", "baseline": %s}' \
               "$n" "${VALUE[$n]}" "${UNIT[$n]}" "${DIR[$n]}" "${BASE[$n]:-null}"
    done
    printf '\n  }\n}\n'
    exit 0
fi

# ── report ───────────────────────────────────────────────────────────────────
echo
echo "Ants perf report — $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "machine: $MACHINE"
[[ "$REPEAT" -gt 1 ]] && echo "repeat:  $REPEAT runs per benchmark, best kept"
case "$COMPARABLE" in
  0) echo "baseline: none at $BASELINE — run with --save-baseline to record one" ;;
  2) echo "baseline: $BASE_DATE ($BASE_COMMIT)"
     echo "          NOT COMPARED — recorded on a different machine:"
     echo "            baseline: $BASE_MACHINE"
     echo "            this run: $MACHINE" ;;
  *) echo "baseline: $BASE_DATE ($BASE_COMMIT), threshold ±${THRESHOLD}%" ;;
esac
[[ "$skipped_count" -gt 0 ]] && { echo; echo "not run:"; printf '  %s\n' "${skipped[@]}"; }
echo

printf '%-42s %12s %-9s %10s\n' "metric" "value" "unit" "vs base"
printf '%s\n' "------------------------------------------------------------------------------"

regressions=0 improvements=0
for n in $(printf '%s\n' "${!VALUE[@]}" | sort); do
    v="${VALUE[$n]}"; u="${UNIT[$n]}"; d="${DIR[$n]}"
    delta="" mark=""
    if [[ "$COMPARABLE" == 1 && -n "${BASE[$n]:-}" ]]; then
        read -r pct verdict < <(awk -v new="$v" -v old="${BASE[$n]}" -v dir="$d" \
                                    -v th="$THRESHOLD" 'BEGIN{
            if (old == 0) { print "0.0 same"; exit }
            pct = (new - old) / old * 100.0;
            better = (dir == "lower_is_better") ? -pct : pct;
            v = (better < -th) ? "worse" : ((better > th) ? "better" : "same");
            printf "%+.1f %s", pct, v;
        }')
        delta="${pct}%"
        case "$verdict" in
            worse)  mark="  REGRESSION"; ((regressions++)) ;;
            better) mark="  improved";   ((improvements++)) ;;
        esac
    elif [[ "$COMPARABLE" == 1 ]]; then
        delta="new"
    fi
    printf '%-42s %12s %-9s %10s%s\n' "$n" "$v" "$u" "$delta" "$mark"
done

echo
if [[ "$COMPARABLE" == 1 ]]; then
    echo "$metric_count metrics — $regressions regression(s), $improvements improvement(s), threshold ±${THRESHOLD}%"
    [[ "$regressions" -gt 0 ]] && exit 1
else
    echo "$metric_count metrics recorded (not compared)"
fi
exit 0
