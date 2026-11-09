#!/usr/bin/env bash
# Fetch a BEIR dataset (corpus + queries + qrels) into tests/eval/data/<name>.
# Idempotent: skips the download if the three files are already present.
#
# Usage: fetch_beir.sh <dataset>      e.g. scifact, nfcorpus
#
# All BEIR datasets share one layout — corpus.jsonl, queries.jsonl,
# qrels/test.tsv — so one fetcher covers them. Pick small, licensing-clean
# tasks: scifact (5183 docs, strong dense first stage) and nfcorpus (3633
# docs, weak lexical first stage where a reranker has room to help).
set -euo pipefail

DATASET="${1:?usage: fetch_beir.sh <dataset>  (e.g. scifact, nfcorpus)}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA="$HERE/data"
URL="https://public.ukp.informatik.tu-darmstadt.de/thakur/BEIR/datasets/${DATASET}.zip"
DEST="$DATA/$DATASET"

# squadit is built from SQuAD-it (Italian), not downloaded as a BEIR zip.
if [ "$DATASET" = "squadit" ]; then
    exec python3 -I "$HERE/fetch_squadit.py"
fi

if [ -f "$DEST/corpus.jsonl" ] && \
   [ -f "$DEST/queries.jsonl" ] && \
   [ -f "$DEST/qrels/test.tsv" ]; then
    echo "fetch_beir: $DATASET already present in $DEST"
    exit 0
fi

mkdir -p "$DATA"
echo "fetch_beir: downloading ${DATASET}.zip ..."
curl -sSL -m 300 -o "$DATA/$DATASET.zip" "$URL"
echo "fetch_beir: extracting ..."
unzip -o -q "$DATA/$DATASET.zip" -d "$DATA"
rm -f "$DATA/$DATASET.zip"
echo "fetch_beir: ready at $DEST"
