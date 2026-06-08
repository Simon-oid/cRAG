#!/usr/bin/env bash
# Tier 4.8 — soak / long-running stability: reindex <-> query cycle.
#
# The crash/hostile suites prove correctness under a single adversarial event.
# THIS proves *stability over time*: that running cRAG's two hot paths in a
# tight loop for hours does not leak, bloat the DB, or drift latency. The thing
# a one-shot test can't catch — an FTS5 index that never checkpoints, a WAL that
# grows without bound, a delete+reinsert churn that fragments the store, a
# per-query allocation that scales with cycle count.
#
# Each cycle:
#   1. `crag reindex` the whole corpus (= index --force: full delete+reinsert,
#      the maximal store churn — re-embeds every file, rewrites every FTS5 row).
#   2. Run a batch of CRAG_SOAK_QUERIES hybrid queries (cosine scan + FTS5 + RRF
#      — the widest query surface), drawn from the Italian answer fixture.
#   3. Sample: DB size (db + -wal + -shm), peak RSS of the reindex process,
#      total + mean query latency.
#
# The store legitimately ramps for the first several reindex cycles before it
# reaches steady state (SQLite frees pages to a reused freelist; FTS5 grows then
# auto-merges its segments), so the first CRAG_SOAK_WARMUP cycles (default 5) are
# a WARM-UP — run, but not used for the baseline. The first post-warmup cycle is
# the baseline. PASS iff, across every cycle after it:
#   * DB size            <= baseline * CRAG_SOAK_DB_TOL   (default 1.5x)
#   * reindex peak RSS   <= baseline * CRAG_SOAK_RSS_TOL  (default 1.5x)
#   * mean query latency <= baseline * CRAG_SOAK_LAT_TOL  (default 2.0x)
# A static corpus + idempotent reindex means chunk count is constant, so once
# warm, a growing DB or RSS is a real leak/bloat — not workload growth. (On the
# 110-file fixture the DB plateaus at ~1.37x its cold cycle-1 size by ~cycle 10
# and never moves again; WAL stays fully checkpointed at 0 B — see TODO 4.8.)
#
# Runs for CRAG_SOAK_SECONDS (default 14400 = 4h); set it small for a smoke run.
# Needs: ollama + bge-m3. sqlite3 CLI optional (chunk-count assertion). Skips
# cleanly if absent. A full per-cycle log is written to $LOG for inspection.
set -u

R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; C=$'\033[36m'; Z=$'\033[0m'
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CRAG="$ROOT/crag"
CORPUS="${CRAG_SOAK_CORPUS:-$HOME/rag-test-data}"
QFILE="${CRAG_SOAK_QFILE:-$ROOT/tests/manual/answers_it.tsv}"
DB="/tmp/crag_soak.db"
LOG="${CRAG_SOAK_LOG:-/tmp/crag_soak.log}"

SECONDS_BUDGET="${CRAG_SOAK_SECONDS:-14400}"     # 4h default
N_QUERIES="${CRAG_SOAK_QUERIES:-100}"
WARMUP="${CRAG_SOAK_WARMUP:-5}"                   # cycles to reach steady state
MAX_CYCLES="${CRAG_SOAK_MAX_CYCLES:-1000000}"    # safety cap
DB_TOL="${CRAG_SOAK_DB_TOL:-1.5}"
RSS_TOL="${CRAG_SOAK_RSS_TOL:-1.5}"
LAT_TOL="${CRAG_SOAK_LAT_TOL:-2.0}"

export CRAG_EMBED_BACKEND="${CRAG_EMBED_BACKEND:-ollama}"
export CRAG_EMBED_MODEL="${CRAG_EMBED_MODEL:-bge-m3}"

[ -x "$CRAG" ] || { echo "${Y}skip:${Z} build crag first (make)"; exit 0; }
[ -d "$CORPUS" ] || { echo "${Y}skip:${Z} corpus $CORPUS not found (set CRAG_SOAK_CORPUS)"; exit 0; }
[ -f "$QFILE" ] || { echo "${Y}skip:${Z} query fixture $QFILE not found"; exit 0; }
if ! curl -sf --max-time 2 http://localhost:11434/api/tags | grep -q "$CRAG_EMBED_MODEL"; then
    echo "${Y}skip:${Z} ollama $CRAG_EMBED_MODEL not available"; exit 0; fi
HAVE_SQLITE=0; command -v sqlite3 >/dev/null && HAVE_SQLITE=1

# Query pool: 4th tab field of the non-comment fixture rows (the question text).
mapfile -t QUERIES < <(grep -v '^#' "$QFILE" | awk -F'\t' 'NF>=4 && $4!=""{print $4}')
[ "${#QUERIES[@]}" -gt 0 ] || { echo "${Y}skip:${Z} no queries parsed from $QFILE"; exit 0; }

cleanup() { rm -f "$DB" "$DB-wal" "$DB-shm"; }
db_bytes() {
    local t=0 f
    for f in "$DB" "$DB-wal" "$DB-shm"; do
        [ -f "$f" ] && t=$((t + $(wc -c < "$f")))
    done
    echo "$t"
}
nchunks() { [ "$HAVE_SQLITE" = 1 ] && sqlite3 "$DB" "SELECT count(*) FROM chunks;" 2>/dev/null || echo "?"; }

# Run "$@" in the background, poll /proc/PID/status for peak VmRSS (kB) until it
# exits; echo the peak (or 0 if /proc is unavailable). Returns the cmd's status.
peak_rss_run() {
    "$@" >/dev/null 2>&1 &
    local pid=$! peak=0 cur
    while kill -0 "$pid" 2>/dev/null; do
        cur=$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status" 2>/dev/null)
        [ -n "${cur:-}" ] && [ "$cur" -gt "$peak" ] 2>/dev/null && peak=$cur
    done
    wait "$pid"; local rc=$?
    PEAK_RSS_KB=$peak
    return $rc
}

# fixed-point "a <= b * tol" using awk (bash has no floats)
within() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN{exit !(a <= b*t)}'; }

echo "corpus:  ${C}$CORPUS${Z} ($(find "$CORPUS" -type f | wc -l | tr -d ' ') files)"
echo "queries: ${C}${#QUERIES[@]}${Z} in pool, ${C}$N_QUERIES${Z}/cycle (hybrid)"
echo "budget:  ${C}$((SECONDS_BUDGET/60))${Z} min   warmup: ${C}$WARMUP${Z} cycles   tolerances: db<=${DB_TOL}x rss<=${RSS_TOL}x lat<=${LAT_TOL}x"
echo "log:     ${C}$LOG${Z}"
: > "$LOG"
echo "cycle	t_s	reindex_s	rss_kb	db_bytes	chunks	q_total_s	q_mean_ms" >> "$LOG"

cleanup
trap 'cleanup' EXIT
start=$(date +%s)
base_db=0 base_rss=0 base_lat=0
fails=0 cycle=0

while :; do
    now=$(date +%s); elapsed=$((now - start))
    [ "$elapsed" -ge "$SECONDS_BUDGET" ] && break
    [ "$cycle" -ge "$MAX_CYCLES" ] && break
    cycle=$((cycle + 1))

    # --- 1. reindex (full churn), timed, with peak RSS ---
    ri_t0=$(date +%s.%N)
    peak_rss_run "$CRAG" reindex "$CORPUS" --db "$DB" --workers 2
    ri_rc=$?
    ri_t1=$(date +%s.%N)
    ri_s=$(awk -v a="$ri_t0" -v b="$ri_t1" 'BEGIN{printf "%.1f", b-a}')
    rss=$PEAK_RSS_KB
    if [ "$ri_rc" != 0 ]; then
        echo "  ${R}cycle $cycle: reindex exited $ri_rc${Z}"; fails=$((fails+1)); break
    fi

    # --- 2. query batch, timed ---
    q_t0=$(date +%s.%N)
    for ((i=0; i<N_QUERIES; i++)); do
        q="${QUERIES[$((i % ${#QUERIES[@]}))]}"
        "$CRAG" query "$q" --db "$DB" --search hybrid -k 5 >/dev/null 2>&1
    done
    q_t1=$(date +%s.%N)
    q_total=$(awk -v a="$q_t0" -v b="$q_t1" 'BEGIN{printf "%.2f", b-a}')
    q_mean_ms=$(awk -v t="$q_total" -v n="$N_QUERIES" 'BEGIN{printf "%.1f", (t/n)*1000}')

    dbb=$(db_bytes); nch=$(nchunks); el=$((  $(date +%s) - start ))
    printf '%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$cycle" "$el" "$ri_s" "$rss" "$dbb" "$nch" "$q_total" "$q_mean_ms" >> "$LOG"

    if [ "$cycle" -le "$WARMUP" ]; then
        printf "  ${Y}warmup${Z} c%d: reindex %ss, rss %sk, db %sB, %s chunks, q_mean %sms\n" \
               "$cycle" "$ri_s" "$rss" "$dbb" "$nch" "$q_mean_ms"
    elif [ "$base_db" = 0 ]; then
        base_db=$dbb; base_rss=$rss; base_lat=$q_mean_ms
        printf "  ${C}baseline${Z} c%d (post-warmup): reindex %ss, rss %sk, db %sB, %s chunks, q_mean %sms\n" \
               "$cycle" "$ri_s" "$rss" "$dbb" "$nch" "$q_mean_ms"
    else
        bad=""
        within "$dbb"      "$base_db"  "$DB_TOL"  || bad="$bad db(${dbb}>${DB_TOL}x${base_db})"
        [ "$base_rss" -gt 0 ] && { within "$rss" "$base_rss" "$RSS_TOL" || bad="$bad rss(${rss}>${RSS_TOL}x${base_rss})"; }
        within "$q_mean_ms" "$base_lat" "$LAT_TOL" || bad="$bad lat(${q_mean_ms}>${LAT_TOL}x${base_lat})"
        if [ -n "$bad" ]; then
            echo "  ${R}cycle $cycle (t=${el}s): DRIFT:$bad${Z}"; fails=$((fails+1))
        else
            printf "  ${G}c%d ok${Z} t=%ss reindex %ss rss %sk db %sB %s chunks q_mean %sms\n" \
                   "$cycle" "$el" "$ri_s" "$rss" "$dbb" "$nch" "$q_mean_ms"
        fi
    fi
done

echo "=== soak: $cycle cycles in $(( $(date +%s) - start ))s, $fails drift/error events ==="
echo "full log: $LOG"
[ "$fails" = 0 ] && { echo "${G}OK${Z}"; exit 0; } || { echo "${R}FAIL${Z}"; exit 1; }
