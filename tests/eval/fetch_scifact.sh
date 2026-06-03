#!/usr/bin/env bash
# Fetch the BEIR SciFact dataset (corpus + queries + qrels) into
# tests/eval/data/scifact. Idempotent: skips download if already present.
#
# SciFact: 5183 corpus docs, ~300 test queries with human relevance
# judgments. Small, licensing-clean, a standard BEIR retrieval task.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA="$HERE/data"
URL="https://public.ukp.informatik.tu-darmstadt.de/thakur/BEIR/datasets/scifact.zip"

if [ -f "$DATA/scifact/corpus.jsonl" ] && \
   [ -f "$DATA/scifact/queries.jsonl" ] && \
   [ -f "$DATA/scifact/qrels/test.tsv" ]; then
    echo "fetch_scifact: already present in $DATA/scifact"
    exit 0
fi

mkdir -p "$DATA"
echo "fetch_scifact: downloading scifact.zip ..."
curl -sSL -m 300 -o "$DATA/scifact.zip" "$URL"
echo "fetch_scifact: extracting ..."
unzip -o -q "$DATA/scifact.zip" -d "$DATA"
rm -f "$DATA/scifact.zip"
echo "fetch_scifact: ready at $DATA/scifact"
