#!/usr/bin/env python3
"""Independent exact-cosine reference for cRAG's retrieval engine.

Reads the SAME document vectors cRAG stored in its SQLite DB, re-embeds the
queries with the SAME ollama model (deterministic), runs an exact brute-force
cosine kNN in numpy, and scores BEIR nDCG@10 / Recall@{10,50,100} with the SAME
logic as tests/eval_beir.c (top-150 chunks -> collapse to unique doc ids
first-seen, up to 100 docs -> binary qrels, score>=1).

If this matches cRAG's own make-eval cosine number to the decimals, then cRAG's
hand-rolled C cosine scan + top-K is a faithful exact search: the ENGINE adds no
retrieval loss, holding embedder + data fixed. This is the rigorous, offline
version of the §2 fidelity claim.

Usage: reference.py <db> <queries.jsonl> <qrels.tsv> <ollama-model>
"""
import sys, json, struct, sqlite3, os, urllib.request
import numpy as np

CHUNK_K, MAX_DOCS, RANK_CUT = 150, 100, 10
CUTS = [10, 50, 100]
OLLAMA = "http://localhost:11434/api/embeddings"


def docid(src):
    base = src.rsplit("/", 1)[-1]
    return base.rsplit(".", 1)[0]


def load_qrels(path):
    rel = {}
    with open(path) as f:
        for i, line in enumerate(f):
            if i == 0 and "query-id" in line:
                continue
            p = line.split()
            if len(p) < 3:
                continue
            qid, did, score = p[0], p[1], int(p[2])
            if score < 1:
                continue
            rel.setdefault(qid, []).append(did)
    return rel


def load_query_text(path, qids):
    text = {}
    with open(path) as f:
        for line in f:
            o = json.loads(line)
            if o["_id"] in qids:
                text[o["_id"]] = o["text"]
    return text


def embed(text, model, cache):
    if text in cache:
        return cache[text]
    req = urllib.request.Request(
        OLLAMA, data=json.dumps({"model": model, "prompt": text}).encode(),
        headers={"Content-Type": "application/json"})
    v = json.load(urllib.request.urlopen(req))["embedding"]
    cache[text] = v
    return v


def main():
    db_path, queries, qrels, model = sys.argv[1:5]
    rel = load_qrels(qrels)
    qids = list(rel.keys())
    qtext = load_query_text(queries, set(qids))

    db = sqlite3.connect(db_path)
    rows = db.execute("select source, dim, vec from chunks").fetchall()
    dim = rows[0][1]
    srcs = [docid(r[0]) for r in rows]
    V = np.empty((len(rows), dim), dtype=np.float32)
    for i, r in enumerate(rows):
        V[i] = struct.unpack(f"<{r[1]}f", r[2])
    # normalize doc vectors for cosine
    V /= (np.linalg.norm(V, axis=1, keepdims=True) + 1e-12)
    srcs = np.array(srcs)

    cache_path = f"/tmp/crag_qcache_{model.replace('/','_')}.json"
    cache = json.load(open(cache_path)) if os.path.exists(cache_path) else {}

    nd_sum = mrr_sum = 0.0
    rec_sum = [0.0, 0.0, 0.0]
    n = 0
    for qid in qids:
        if qid not in qtext:
            continue
        q = np.array(embed(qtext[qid], model, cache), dtype=np.float32)
        q /= (np.linalg.norm(q) + 1e-12)
        sims = V @ q
        topk = np.argpartition(-sims, CHUNK_K)[:CHUNK_K]
        topk = topk[np.argsort(-sims[topk])]
        # collapse chunks -> unique doc ids, first-seen, up to MAX_DOCS
        docs, seen = [], set()
        for idx in topk:
            d = srcs[idx]
            if d not in seen:
                seen.add(d); docs.append(d)
                if len(docs) >= MAX_DOCS:
                    break
        rset = set(rel[qid]); nrel = len(rset)
        cut = min(len(docs), RANK_CUT)
        dcg = 0.0; first = 0
        for i in range(cut):
            if docs[i] in rset:
                dcg += 1.0 / np.log2(i + 2)
                if not first:
                    first = i + 1
        ideal = min(nrel, RANK_CUT)
        idcg = sum(1.0 / np.log2(i + 2) for i in range(ideal))
        nd_sum += dcg / idcg if idcg > 0 else 0.0
        mrr_sum += 1.0 / first if first else 0.0
        for c, depth in enumerate(CUTS):
            d = min(depth, len(docs))
            hits = sum(1 for i in range(d) if docs[i] in rset)
            rec_sum[c] += hits / nrel if nrel > 0 else 0.0
        n += 1

    json.dump(cache, open(cache_path, "w"))
    print(f"{model} on {os.path.basename(db_path)}  (n={n} queries)")
    print(f"  nDCG@10   = {nd_sum/n:.4f}")
    print(f"  MRR@10    = {mrr_sum/n:.4f}")
    print(f"  Recall@10 = {rec_sum[0]/n:.4f}")
    print(f"  Recall@50 = {rec_sum[1]/n:.4f}")
    print(f"  Recall@100= {rec_sum[2]/n:.4f}")


if __name__ == "__main__":
    main()
