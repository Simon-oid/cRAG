#!/usr/bin/env bash
# Tier 4.4 — crash safety, real fork + SIGKILL integration test.
#
# The offline counterpart (tests/test_crash_safe.c, in `make check`) proves the
# transaction/resume invariants deterministically. THIS proves the same property
# against the actual `crag index` process killed with SIGKILL mid-run, over a
# real corpus and a live embedder — the thing a deterministic test can't fully
# stand in for (signal timing, WAL recovery on a real on-disk DB, the worker
# pool dying mid-embed).
#
# Per iteration: start `crag index`, SIGKILL it at a random delay, then assert
#   1. PRAGMA integrity_check == ok        (no DB corruption)
#   2. no orphan chunks / no empty files   (chunks<->files consistent)
#   3. resume run completes (exit 0)
#   4. final chunk/file counts match a clean from-scratch index (idempotent)
#
# Needs: ollama + bge-m3, sqlite3 CLI. Skips cleanly if absent.
set -u

R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; C=$'\033[36m'; Z=$'\033[0m'
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CRAG="$ROOT/crag"
CORPUS="${CRAG_CRASH_CORPUS:-$HOME/rag-test-data}"
DB="/tmp/crag_crash_safe.db"
ITERS="${CRAG_CRASH_ITERS:-3}"
export CRAG_EMBED_BACKEND=ollama CRAG_EMBED_MODEL=bge-m3

command -v sqlite3 >/dev/null || { echo "${Y}skip:${Z} no sqlite3 CLI"; exit 0; }
[ -x "$CRAG" ] || { echo "${Y}skip:${Z} build crag first (make)"; exit 0; }
[ -d "$CORPUS" ] || { echo "${Y}skip:${Z} corpus $CORPUS not found (set CRAG_CRASH_CORPUS)"; exit 0; }
if ! curl -sf --max-time 2 http://localhost:11434/api/tags | grep -q bge-m3; then
    echo "${Y}skip:${Z} ollama bge-m3 not available"; exit 0; fi

cleanup() { rm -f "$DB" "$DB-wal" "$DB-shm"; }
integrity() { sqlite3 "$DB" "PRAGMA integrity_check;" 2>/dev/null; }
orphans()   { sqlite3 "$DB" "SELECT count(*) FROM chunks WHERE source NOT IN (SELECT path FROM files);" 2>/dev/null; }
empties()   { sqlite3 "$DB" "SELECT count(*) FROM files WHERE path NOT IN (SELECT DISTINCT source FROM chunks);" 2>/dev/null; }
nchunks()   { sqlite3 "$DB" "SELECT count(*) FROM chunks;" 2>/dev/null; }
nfiles()    { sqlite3 "$DB" "SELECT count(*) FROM files;" 2>/dev/null; }

echo "corpus: ${C}$CORPUS${Z} ($(find "$CORPUS" -type f | wc -l | tr -d ' ') files)   iters: ${C}$ITERS${Z}"

# --- baseline: a clean, uninterrupted index defines the correct final state ---
cleanup
"$CRAG" index "$CORPUS" --db "$DB" --workers 2 >/dev/null 2>&1
BASE_C=$(nchunks); BASE_F=$(nfiles)
echo "baseline (clean): ${C}$BASE_C${Z} chunks, ${C}$BASE_F${Z} files"

fails=0
for i in $(seq 1 "$ITERS"); do
    cleanup
    # start the index, SIGKILL it mid-run
    "$CRAG" index "$CORPUS" --db "$DB" --workers 2 >/dev/null 2>&1 &
    pid=$!
    delay=$(awk -v s="$i" 'BEGIN{srand(s*7+1); printf "%.1f", 1.5 + rand()*5.0}')
    sleep "$delay"
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    killed_c=$(nchunks)

    ok=1
    ig=$(integrity)
    [ "$ig" = "ok" ] || { echo "  ${R}iter $i: integrity_check='$ig'${Z}"; ok=0; }
    [ "$(orphans)" = "0" ] || { echo "  ${R}iter $i: $(orphans) orphan chunks${Z}"; ok=0; }
    [ "$(empties)" = "0" ] || { echo "  ${R}iter $i: $(empties) empty file rows${Z}"; ok=0; }

    # resume to completion (must exit 0) and reach the baseline state
    if ! "$CRAG" index "$CORPUS" --db "$DB" --workers 2 >/dev/null 2>&1; then
        echo "  ${R}iter $i: resume index failed${Z}"; ok=0; fi
    rc=$(nchunks); rf=$(nfiles)
    [ "$(integrity)" = "ok" ] || { echo "  ${R}iter $i: post-resume integrity bad${Z}"; ok=0; }
    [ "$rc" = "$BASE_C" ] || { echo "  ${R}iter $i: resumed chunks $rc != baseline $BASE_C${Z}"; ok=0; }
    [ "$rf" = "$BASE_F" ] || { echo "  ${R}iter $i: resumed files $rf != baseline $BASE_F${Z}"; ok=0; }

    if [ "$ok" = 1 ]; then
        printf "  ${G}iter %d PASS${Z}  killed@%ss (%s/%s chunks), integrity ok, resumed -> %s/%s\n" \
               "$i" "$delay" "$killed_c" "$BASE_C" "$rc" "$rf"
    else
        fails=$((fails+1))
    fi
done

cleanup
echo "=== crash-safety: $((ITERS-fails))/$ITERS iterations passed ==="
[ "$fails" = 0 ] && echo "${G}OK${Z}" || { echo "${R}FAIL${Z}"; exit 1; }
