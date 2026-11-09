#!/usr/bin/env python3
"""Build an Italian retrieval benchmark from SQuAD-it, in BEIR layout.

SQuAD-it (Croce et al., 2018; github.com/crux82/squad-it) is SQuAD v1.1
translated into Italian: questions, each written against one Wikipedia
paragraph. Turned into retrieval, the paragraphs are the corpus and each
question's own paragraph is its one relevant document. That gives a
gradable Italian set with no hand-labelling: the relevance comes from
how the dataset was built, not from us.

Output, the layout tests/eval/eval_beir reads:
    tests/eval/data/squadit/corpus.jsonl     {"_id", "title", "text"}
    tests/eval/data/squadit/queries.jsonl    {"_id", "text"}
    tests/eval/data/squadit/qrels/test.tsv   query-id  corpus-id  score

All 2,010 test-split paragraphs form the corpus; a fixed, seeded sample of
SAMPLE questions keeps a full run short. Idempotent.

Caveat, stated in docs/benchmarks.md: binary relevance with exactly one
relevant paragraph per question, and machine-assisted translation, so
absolute numbers are not comparable with English BEIR tasks; the
cosine / bm25 / hybrid comparison and the embedder comparison are the
signal.
"""

from __future__ import annotations

import gzip
import json
import random
import sys
import urllib.request
from pathlib import Path

URL = "https://github.com/crux82/squad-it/raw/master/SQuAD_it-test.json.gz"
SAMPLE = 500
SEED = 20261109

DEST = Path(__file__).resolve().parent / "data" / "squadit"


def main() -> int:
    files = [DEST / "corpus.jsonl", DEST / "queries.jsonl", DEST / "qrels" / "test.tsv"]
    if all(f.exists() for f in files):
        print(f"fetch_squadit: already present in {DEST}")
        return 0

    print("fetch_squadit: downloading SQuAD-it test split ...")
    with urllib.request.urlopen(URL, timeout=120) as r:
        data = json.loads(gzip.decompress(r.read()))

    corpus, questions = [], []
    for a, article in enumerate(data["data"]):
        title = article.get("title", "").replace("_", " ")
        for p, para in enumerate(article["paragraphs"]):
            doc_id = f"a{a}p{p}"
            corpus.append({"_id": doc_id, "title": title, "text": para["context"]})
            for qa in para["qas"]:
                questions.append((qa["id"], qa["question"].strip(), doc_id))

    rng = random.Random(SEED)
    sample = sorted(rng.sample(questions, min(SAMPLE, len(questions))))

    (DEST / "qrels").mkdir(parents=True, exist_ok=True)
    with (DEST / "corpus.jsonl").open("w") as f:
        for d in corpus:
            f.write(json.dumps(d, ensure_ascii=False) + "\n")
    with (DEST / "queries.jsonl").open("w") as f:
        for qid, text, _ in sample:
            f.write(json.dumps({"_id": qid, "text": text}, ensure_ascii=False) + "\n")
    with (DEST / "qrels" / "test.tsv").open("w") as f:
        f.write("query-id\tcorpus-id\tscore\n")
        for qid, _, doc_id in sample:
            f.write(f"{qid}\t{doc_id}\t1\n")
    print(f"fetch_squadit: {len(corpus)} paragraphs, {len(sample)} of "
          f"{len(questions)} questions (seed {SEED}) in {DEST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
