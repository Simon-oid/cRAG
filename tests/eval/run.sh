#!/usr/bin/env bash
# Run the cRAG retrieval-quality eval against BEIR SciFact end to end:
#   fetch dataset -> materialize docs -> crag index -> eval_beir -> table+CSV
#
# Env knobs:
#   CRAG_EMBED_BACKEND   embedder backend   (default: ollama)
#   CRAG_EMBED_MODEL     embedding model    (default: mxbai-embed-large)
#   CRAG_EVAL_DB         index db path      (default: /tmp/crag_scifact.db)
#   CRAG_EVAL_DOCS       materialized dir   (default: /tmp/crag_scifact_docs)
#   CRAG_EVAL_WORKERS    index workers      (default: 4)
#   CRAG_EVAL_FRESH=1    force re-materialize + re-index from scratch
#   CRAG_EVAL_RERANK=1   add a cross-encoder rerank pass as a 4th mode
#   CRAG_RERANK_URL      rerank server endpoint (default: localhost:8090/rerank)
#   CRAG_RERANK_MODEL    rerank model name passed in the request body
#   CRAG_EVAL_RERANK_POOL  hybrid pool depth fed to the reranker (default: 100)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATA="$ROOT/tests/eval/data/scifact"
DB="${CRAG_EVAL_DB:-/tmp/crag_scifact.db}"
DOCS="${CRAG_EVAL_DOCS:-/tmp/crag_scifact_docs}"
WORKERS="${CRAG_EVAL_WORKERS:-4}"
CSV="$ROOT/tests/eval/results.csv"

export CRAG_EMBED_BACKEND="${CRAG_EMBED_BACKEND:-ollama}"
export CRAG_EMBED_MODEL="${CRAG_EMBED_MODEL:-mxbai-embed-large}"

cd "$ROOT"
bash tests/eval/fetch_scifact.sh
make --no-print-directory crag tests/eval_beir

if [ "${CRAG_EVAL_FRESH:-0}" = "1" ]; then
    rm -rf "$DOCS" "$DB" "$DB-wal" "$DB-shm"
fi

# Materialize once: 5183 docs is the expected count.
need_materialize=1
if [ -d "$DOCS" ]; then
    have=$(find "$DOCS" -name '*.txt' | wc -l)
    [ "$have" -ge 5000 ] && need_materialize=0
fi
[ "$need_materialize" = 1 ] && ./tests/eval_beir materialize "$DATA/corpus.jsonl" "$DOCS"

echo "indexing with $CRAG_EMBED_BACKEND/$CRAG_EMBED_MODEL ($WORKERS workers) ..."
time ./crag index "$DOCS" --db "$DB" --ext txt --workers "$WORKERS"

rm -f "$CSV"
EVAL_ARGS=("$DB" "$DATA/queries.jsonl" "$DATA/qrels/test.tsv" --csv "$CSV")
if [ "${CRAG_EVAL_RERANK:-0}" = "1" ]; then
    EVAL_ARGS+=(--rerank --rerank-pool "${CRAG_EVAL_RERANK_POOL:-100}")
    [ -n "${CRAG_RERANK_URL:-}" ] && EVAL_ARGS+=(--rerank-url "$CRAG_RERANK_URL")
    echo "rerank pass enabled (url=${CRAG_RERANK_URL:-default}, model=${CRAG_RERANK_MODEL:-reranker})"
fi
./tests/eval_beir eval "${EVAL_ARGS[@]}"
