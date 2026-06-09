#!/usr/bin/env python3
"""Three-way exact-search fidelity: cRAG (C) vs NumPy vs FAISS IndexFlatIP.

Reads cRAG's stored doc vectors, re-embeds queries with the same ollama model
(deterministic), and runs the SAME exact-cosine retrieval + BEIR scoring two
independent ways — NumPy matmul and FAISS IndexFlatIP (Facebook AI's standard
exact searcher). Prints nDCG@10 for both. Same logic as tests/eval_beir.c.

Usage: reference_faiss.py <db> <queries.jsonl> <qrels.tsv> <ollama-model>
"""
import sys, json, struct, sqlite3, os, urllib.request
import numpy as np
import faiss

CHUNK_K, MAX_DOCS, RANK_CUT = 150, 100, 10
CUTS = [10, 50, 100]
OLLAMA = "http://localhost:11434/api/embeddings"


def docid(src):
    return src.rsplit("/", 1)[-1].rsplit(".", 1)[0]


def load_qrels(path):
    rel = {}
    with open(path) as f:
        for i, line in enumerate(f):
            if i == 0 and "query-id" in line:
                continue
            p = line.split()
            if len(p) < 3:
                continue
            if int(p[2]) < 1:
                continue
            rel.setdefault(p[0], []).append(p[1])
    return rel


def load_qtext(path, qids):
    t = {}
    with open(path) as f:
        for line in f:
            o = json.loads(line)
            if o["_id"] in qids:
                t[o["_id"]] = o["text"]
    return t


def embed(text, model, cache):
    if text in cache:
        return cache[text]
    req = urllib.request.Request(
        OLLAMA, data=json.dumps({"model": model, "prompt": text}).encode(),
        headers={"Content-Type": "application/json"})
    v = json.load(urllib.request.urlopen(req))["embedding"]
    cache[text] = v
    return v


def score(ranked_docs, rset, nrel):
    cut = min(len(ranked_docs), RANK_CUT)
    dcg = 0.0; first = 0
    for i in range(cut):
        if ranked_docs[i] in rset:
            dcg += 1.0 / np.log2(i + 2)
            if not first:
                first = i + 1
    idcg = sum(1.0 / np.log2(i + 2) for i in range(min(nrel, RANK_CUT)))
    nd = dcg / idcg if idcg > 0 else 0.0
    return nd


def collapse(order, srcs):
    docs, seen = [], set()
    for idx in order:
        d = srcs[idx]
        if d not in seen:
            seen.add(d); docs.append(d)
            if len(docs) >= MAX_DOCS:
                break
    return docs


def main():
    db_path, queries, qrels, model = sys.argv[1:5]
    rel = load_qrels(qrels)
    qids = list(rel.keys())
    qtext = load_qtext(queries, set(qids))

    db = sqlite3.connect(db_path)
    rows = db.execute("select source, dim, vec from chunks").fetchall()
    dim = rows[0][1]
    srcs = np.array([docid(r[0]) for r in rows])
    V = np.empty((len(rows), dim), dtype=np.float32)
    for i, r in enumerate(rows):
        V[i] = struct.unpack(f"<{r[1]}f", r[2])
    V /= (np.linalg.norm(V, axis=1, keepdims=True) + 1e-12)

    index = faiss.IndexFlatIP(dim)
    index.add(V)

    cache_path = f"/tmp/crag_qcache_{model.replace('/','_')}.json"
    cache = json.load(open(cache_path)) if os.path.exists(cache_path) else {}

    nd_np = nd_fa = 0.0
    n = 0
    for qid in qids:
        if qid not in qtext:
            continue
        q = np.array(embed(qtext[qid], model, cache), dtype=np.float32)
        q /= (np.linalg.norm(q) + 1e-12)
        rset = set(rel[qid]); nrel = len(rset)

        # NumPy exact
        sims = V @ q
        tk = np.argpartition(-sims, CHUNK_K)[:CHUNK_K]
        tk = tk[np.argsort(-sims[tk])]
        nd_np += score(collapse(tk, srcs), rset, nrel)

        # FAISS IndexFlatIP exact
        _, I = index.search(q.reshape(1, -1), CHUNK_K)
        nd_fa += score(collapse(I[0], srcs), rset, nrel)
        n += 1

    json.dump(cache, open(cache_path, "w"))
    print(f"{nd_np/n:.4f} {nd_fa/n:.4f}")


if __name__ == "__main__":
    main()
