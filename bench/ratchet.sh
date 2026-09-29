#!/usr/bin/env bash
# End-to-end "what the user feels" performance ratchet for bro.
#
# Runs a small set of probes against the built bro-headless (and tats's own
# headless binary when ../tats is built), takes the median of several
# repetitions per number, and compares each with the checked-in golden in
# bench/goldens.<platform>.json: a number fails only when it is worse than its
# golden by more than its margin. Prints a table and an overall PASS/FAIL
# (exit 1 on FAIL). See docs/perf-ratchet.md.
#
# usage: bench/ratchet.sh [--update] [--force] [--reps N] [--only LIST]
#                         [--work DIR] [--bin PATH] [--app DIR] [--tats DIR]
#                         [--goldens FILE]
#   --update     write this run's medians as the new goldens (refused when the
#                machine was busy, unless --force)
#   --only LIST  comma-separated subset of: cpu,gc,compile,tats
#   --work DIR   where probe logs and the throwaway cold code caches go
#                (default: a fresh directory under $TMPDIR); never the user's
#                code cache
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPS=5
UPDATE=0
FORCE=0
ONLY="cpu,gc,compile,tats"
WORK="${BRO_RATCHET_WORK:-}"
BIN="${BRO_HEADLESS:-}"
GOLDENS=""
APP="${BRO_RATCHET_APP:-$ROOT/../broworkshop/tools/scene-editor}"
TATS="${BRO_RATCHET_TATS:-$ROOT/../tats}"
BUSY_PERCENT=20
GC_SIZES="20000 500000 1500000"
PROBE_TIMEOUT=600

while [ $# -gt 0 ]; do
  case "$1" in
    --update) UPDATE=1 ;;
    --force) FORCE=1 ;;
    --reps) REPS=$2; shift ;;
    --only) ONLY=$2; shift ;;
    --work) WORK=$2; shift ;;
    --bin) BIN=$2; shift ;;
    --app) APP=$2; shift ;;
    --tats) TATS=$2; shift ;;
    --goldens) GOLDENS=$2; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) PLATFORM=windows; EXE=.exe ;;
  Darwin) PLATFORM=macos; EXE= ;;
  *) PLATFORM=linux; EXE= ;;
esac

if [ -z "$BIN" ]; then
  if [ "$PLATFORM" = windows ]; then BIN="$ROOT/build/Release/bro-headless.exe"
  else BIN="$ROOT/build-release/bro-headless"; fi
fi
if [ ! -x "$BIN" ]; then echo "bro-headless not found at $BIN (build it or pass --bin)" >&2; exit 2; fi

GOLDENS="${GOLDENS:-$ROOT/bench/goldens.$PLATFORM.json}"
PROBES="$ROOT/bench/probes"
if [ -z "$WORK" ]; then WORK=$(mktemp -d "${TMPDIR:-/tmp}/bro-ratchet.XXXXXX"); fi
mkdir -p "$WORK"

wants() { case ",$ONLY," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }
now_ms() { date +%s%3N; }

# ---------------------------------------------------------------------------
# Machine load: percent of all logical CPUs busy over a few seconds, sampled
# while no probe runs. -1 when the platform gives no reading.
# ---------------------------------------------------------------------------
machine_load() {
  case "$PLATFORM" in
    windows)
      typeperf "\\Processor(_Total)\\% Processor Time" -sc 4 2>/dev/null |
        awk -F'"' 'NR > 2 && $4 ~ /^[0-9.]+$/ { s += $4; n++ } END { if (n) printf "%.1f", s / n; else print -1 }' ;;
    linux)
      local a b
      a=$(awk '/^cpu / { t = 0; for (i = 2; i <= NF; i++) t += $i; print t, $5 + $6 }' /proc/stat)
      sleep 2
      b=$(awk '/^cpu / { t = 0; for (i = 2; i <= NF; i++) t += $i; print t, $5 + $6 }' /proc/stat)
      echo "$a $b" | awk '{ dt = $3 - $1; di = $4 - $2; if (dt > 0) printf "%.1f", 100 * (dt - di) / dt; else print -1 }' ;;
    *) echo -1 ;;
  esac
}

report_load() {
  local when=$1 pct=$2
  if [ "$pct" = "-1" ]; then
    echo "[LOAD] $when: no CPU load reading on this platform; cannot vouch for a quiet machine."
  elif awk "BEGIN { exit !($pct > $BUSY_PERCENT) }"; then
    echo "[LOAD] $when: ${pct}% busy (> ${BUSY_PERCENT}%)."
    echo "[LOAD] WARNING: MACHINE IS BUSY. Timings from this run are untrustworthy."
    MACHINE_BUSY=1
  else
    echo "[LOAD] $when: ${pct}% busy (quiet; threshold ${BUSY_PERCENT}%)."
  fi
}

# ---------------------------------------------------------------------------
# Samples: SAMPLES[key] is a space-separated list of this run's values.
# ---------------------------------------------------------------------------
declare -A SAMPLES
declare -a ORDER
declare -A SKIPPED
add_sample() {
  local key=$1 val=$2
  [ -z "$val" ] && return
  if [ -z "${SAMPLES[$key]+x}" ]; then ORDER+=("$key"); SAMPLES[$key]=""; fi
  SAMPLES[$key]="${SAMPLES[$key]} $val"
}
skip_group() { SKIPPED[$1]="$2"; echo "[SKIP] $1: $2"; }

stat_of() { # median|min|max of the key's samples
  echo "${SAMPLES[$2]}" | tr ' ' '\n' | grep -v '^$' | sort -g |
    awk -v what="$1" '{ v[NR] = $1 } END {
      if (!NR) { print ""; exit }
      if (what == "min") print v[1]; else if (what == "max") print v[NR];
      else if (NR % 2) print v[(NR + 1) / 2]; else print (v[NR / 2] + v[NR / 2 + 1]) / 2 }'
}

# Per-key regression margin (fraction). Every number is a time in ms, lower
# is better. The default holds for anything whose median moves under ~4%
# between quiet runs; keys that swing more carry a wider margin (spreads in
# docs/perf-ratchet.md) so a quiet run stays green. The boot wall times
# (process launch, GPU context, exit) moved up to 25% between quiet
# invocations, so they only catch large regressions.
margin_for() {
  case "$1" in
    gc_*_worst_ms) echo 0.25 ;;
    cpu_strings_ms) echo 0.20 ;;
    tats_*) echo 0.15 ;;
    page_compile_*) echo 0.15 ;;
    boot_*) echo 0.25 ;;
    *) echo 0.10 ;;
  esac
}

golden_of() {
  [ -f "$GOLDENS" ] || return
  awk -v k="\"$1\"" -F':' '$1 ~ k { gsub(/[ ,\t\r]/, "", $2); print $2; exit }' "$GOLDENS"
}

run_logged() { # run_logged <log> <cmd...>: runs with a timeout, output to <log>
  local log=$1; shift
  timeout -s KILL "$PROBE_TIMEOUT" "$@" > "$log" 2>&1
}

# ---------------------------------------------------------------------------
# Probes
# ---------------------------------------------------------------------------
probe_cpu() {
  local i log line
  for ((i = 1; i <= REPS; i++)); do
    log="$WORK/cpu_$i.log"
    run_logged "$log" "$BIN" "$PROBES/blank" "$PROBES/cpu_work.js"
    line=$(grep -o 'CPU nbody=.*' "$log" | head -1)
    if [ -z "$line" ]; then echo "[cpu] rep $i: no CPU line (see $log)"; continue; fi
    add_sample cpu_nbody_ms "$(echo "$line" | sed -E 's/.*nbody=([0-9]+)ms.*/\1/')"
    add_sample cpu_toplevel_ms "$(echo "$line" | sed -E 's/.*toplevel=([0-9]+)ms.*/\1/')"
    add_sample cpu_strings_ms "$(echo "$line" | sed -E 's/.*strings=([0-9]+)ms.*/\1/')"
    add_sample cpu_total_ms "$(echo "$line" | sed -E 's/.*total=([0-9]+)ms.*/\1/')"
    echo "[cpu] rep $i: $line" | cut -c1-110
  done
}

probe_gc() {
  local n i log line
  for n in $GC_SIZES; do
    for ((i = 1; i <= REPS; i++)); do
      log="$WORK/gc_${n}_$i.log"
      run_logged "$log" "$BIN" "$PROBES/blank" "$PROBES/gc_pause.js" -- "$n"
      line=$(grep -o 'GCPROBE live=.*' "$log" | head -1)
      if [ -z "$line" ]; then echo "[gc] N=$n rep $i: no GCPROBE line (see $log)"; continue; fi
      add_sample "gc_${n}_worst_ms" "$(echo "$line" | sed -E 's/.*worst=([0-9.]+).*/\1/')"
      add_sample "gc_${n}_avg_ms" "$(echo "$line" | sed -E 's/.*avg=([0-9.]+).*/\1/')"
      echo "[gc] N=$n rep $i: $line"
    done
  done
}

# Cold: an empty code cache of its own, so the page compiles from source and
# stores. Warm: the same cache again, so the page loads from it. The page
# compile is bro's own "compiled <page> in N ms" line; boot is the wall time
# from launching the process to its exit after a one-line driver.
probe_compile() {
  if [ ! -f "$APP/index.html" ]; then skip_group compile "no app at $APP"; return; fi
  local i dir log t0 t1 ms kind lines
  for ((i = 1; i <= REPS; i++)); do
    dir="$WORK/codecache_$i"
    rm -rf "$dir"; mkdir -p "$dir"
    for kind in cold warm; do
      log="$WORK/compile_${kind}_$i.log"
      t0=$(now_ms)
      BRO_CODE_CACHE_DIR="$dir" run_logged "$log" "$BIN" "$APP" -e "0"
      t1=$(now_ms)
      # The page's own compile units (its classic scripts, each module entry),
      # not the system panels' or the -e driver's.
      lines=$(grep -E "compiled .* in [0-9.]+ ms \(code cache" "$log" | grep -v -E "compiled system[/\\\\]|compiled <eval>")
      ms=$(echo "$lines" | sed -nE 's/.* in ([0-9.]+) ms.*/\1/p' | awk '{ s += $1; n++ } END { if (n) print s }')
      if [ -z "$ms" ]; then echo "[compile] $kind rep $i: no compile line (see $log)"; continue; fi
      if [ "$kind" = cold ] && echo "$lines" | grep -q "cache hit"; then
        echo "[compile] cold rep $i hit the cache (see $log)"
      fi
      if [ "$kind" = warm ] && echo "$lines" | grep -q "cache miss"; then
        echo "[compile] warm rep $i missed the cache (see $log)"
      fi
      add_sample "page_compile_${kind}_ms" "$ms"
      add_sample "boot_${kind}_ms" "$((t1 - t0))"
      echo "[compile] $kind rep $i: page compile ${ms} ms, launch to exit $((t1 - t0)) ms"
    done
    rm -rf "$dir"
  done
}

# tats's startup probe, run by its own headless binary against its own code
# cache under $WORK (one priming run, then the timed ones, all warm).
# Nothing in the tats tree is written to: the process runs from $WORK.
probe_tats() {
  local exe probe cache i log
  exe="$TATS/build/Release/tats-headless$EXE"
  [ -x "$exe" ] || exe="$TATS/build/tats-headless$EXE"
  probe="$TATS/tools/startupprobe.js"
  if [ ! -x "$exe" ] || [ ! -f "$probe" ]; then skip_group tats "no built tats at $TATS"; return; fi
  cache="$WORK/codecache_tats"
  mkdir -p "$cache"
  for ((i = 0; i <= REPS; i++)); do
    log="$WORK/tats_$i.log"
    (cd "$WORK" && BRO_CODE_CACHE_DIR="$cache" run_logged "$log" "$exe" "$TATS" "$probe" -- 424242 shattered-city nowrap)
    if ! grep -q "\[STARTUP\] DONE" "$log"; then echo "[tats] rep $i: probe did not finish (see $log)"; continue; fi
    if [ "$i" -eq 0 ]; then echo "[tats] priming run done"; continue; fi
    local boot world mission lab
    boot=$(grep -o 'binary boot to window.tats: [0-9]* ms' "$log" | grep -o '[0-9]* ms' | grep -o '[0-9]*')
    world=$(grep -E '\[STARTUP\] survey WORLD entry +[0-9]+ ms' "$log" | sed -E 's/.*entry +([0-9]+) ms.*/\1/')
    mission=$(grep -E '\[STARTUP\] mission launch +[0-9]+ ms' "$log" | sed -E 's/.*launch +([0-9]+) ms.*/\1/')
    lab=$(grep -E '\[STARTUP\] lab battle +[0-9]+ ms' "$log" | sed -E 's/.*battle +([0-9]+) ms.*/\1/')
    add_sample tats_boot_ms "$boot"
    add_sample tats_world_entry_ms "$world"
    add_sample tats_mission_launch_ms "$mission"
    add_sample tats_lab_battle_ms "$lab"
    echo "[tats] rep $i: boot $boot, WORLD entry $world, mission launch $mission, lab battle $lab ms"
  done
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
MACHINE_BUSY=0
echo "bro end-to-end ratchet: $PLATFORM, $REPS reps, probes $ONLY"
echo "binary: $BIN ($(date -r "$BIN" '+%Y-%m-%d %H:%M' 2>/dev/null))"
echo "bro: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)$(git -C "$ROOT" diff --quiet 2>/dev/null || echo -dirty)"
echo "goldens: $GOLDENS"
echo "logs: $WORK"
report_load "before run" "$(machine_load)"

START=$(now_ms)
wants cpu && probe_cpu
wants gc && probe_gc
wants compile && probe_compile
wants tats && probe_tats
END=$(now_ms)

report_load "after run" "$(machine_load)"
echo "probes took $(((END - START) / 1000)) s"

# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------
FAILS=0
NEWS=0
SUMMARY="$WORK/summary.txt"
: > "$SUMMARY"
echo
printf '%-26s %6s %10s %10s %10s %-19s %9s  %s\n' Key Margin Golden Bound Median "Run [min-max]" "vs gold" Status
printf '%s\n' "--------------------------------------------------------------------------------------------------------------"
for key in "${ORDER[@]}"; do
  med=$(stat_of median "$key"); lo=$(stat_of min "$key"); hi=$(stat_of max "$key")
  margin=$(margin_for "$key")
  gold=$(golden_of "$key")
  if [ -z "$gold" ]; then
    status="[NEW]"; bound="-"; delta="-"; gold="-"; NEWS=$((NEWS + 1))
  else
    bound=$(awk "BEGIN { printf \"%.1f\", $gold * (1 + $margin) }")
    delta=$(awk "BEGIN { printf \"%+.1f%%\", 100 * ($med / $gold - 1) }")
    if awk "BEGIN { exit !($med <= $bound) }"; then status="[PASS]"; else status="[FAIL]"; FAILS=$((FAILS + 1)); fi
  fi
  pct=$(awk "BEGIN { printf \"%d%%\", $margin * 100 + 0.5 }")
  printf '%-26s %6s %10s %10s %10s %-19s %9s  %s\n' "$key" "$pct" "$gold" "$bound" "$med" "[$lo-$hi]" "$delta" "$status"
  echo "$key $med $lo $hi $gold $status" >> "$SUMMARY"
done
for g in "${!SKIPPED[@]}"; do
  printf '%-26s %s\n' "$g" "[SKIP] ${SKIPPED[$g]}"
done
printf '%s\n' "--------------------------------------------------------------------------------------------------------------"

if [ "$UPDATE" = 1 ]; then
  if [ "$MACHINE_BUSY" = 1 ] && [ "$FORCE" != 1 ]; then
    echo "[RATCHET] refusing to --update from a run on a busy machine (pass --force to override)." >&2
    exit 1
  fi
  # Keep goldens this run did not measure (a skipped probe group), replace
  # the rest with this run's medians.
  tmp="$GOLDENS.tmp"
  {
    declare -A merged
    if [ -f "$GOLDENS" ]; then
      while IFS= read -r l; do
        k=$(echo "$l" | sed -nE 's/^ *"([^"]+)": *([0-9.]+).*/\1/p')
        v=$(echo "$l" | sed -nE 's/^ *"([^"]+)": *([0-9.]+).*/\2/p')
        [ -n "$k" ] && merged[$k]=$v
      done < "$GOLDENS"
    fi
    for key in "${ORDER[@]}"; do merged[$key]=$(stat_of median "$key"); done
    keys=$(printf '%s\n' "${!merged[@]}" | sort)
    n=$(echo "$keys" | wc -l)
    echo "{"
    j=0
    for k in $keys; do
      j=$((j + 1))
      sep=","; [ "$j" -eq "$n" ] && sep=""
      echo "  \"$k\": ${merged[$k]}$sep"
    done
    echo "}"
  } > "$tmp" && mv "$tmp" "$GOLDENS"
  echo "[RATCHET] wrote ${#ORDER[@]} goldens to $GOLDENS"
  exit 0
fi

[ "$NEWS" -gt 0 ] && echo "[RATCHET] $NEWS key(s) have no golden yet; --update records them."
[ "$MACHINE_BUSY" = 1 ] && echo "[LOAD] WARNING: the machine was busy; treat this verdict as untrustworthy."
if [ "$FAILS" -gt 0 ]; then
  echo "[RATCHET] FAIL: $FAILS key(s) regressed past their margin."
  exit 1
fi
echo "[RATCHET] PASS: no key regressed past its margin (${#ORDER[@]} keys)."
exit 0
