# Known limitations

Where cRAG fails or stops, each with the measurement behind it when there is
one. Unless noted: Ryzen 7 5800X, Ollama 0.40.4 embedders, measured
2026-11-08/09.

## Scale and speed (by design)

- **Exact search scans every vector.** There is no approximate index (no
  HNSW, no IVF, no quantization). That is a choice: exact, deterministic,
  byte-identical results ([`ROADMAP.md`](ROADMAP.md#out-of-scope-by-design)).
- **It is slower than NumPy and FAISS on the same scan:**

  | chunks | cRAG P50 | NumPy P50 | FAISS `IndexFlatIP` P50 |
  |---|---|---|---|
  | 10,000 | 10.5 ms | 0.5 ms | 1.1 ms |
  | 100,000 | 112 ms | 15.4 ms | 10.5 ms |

  The scan reads SQLite BLOBs without SIMD. It returns the same top-10 as
  NumPy for every query
  ([`docs/bench-vs-numpy-faiss.md`](docs/bench-vs-numpy-faiss.md)). Up to
  about 100k chunks this is fine for interactive use; past that it is the
  wrong tool.
- **Memory grows with the index:** the benchmark process peaked at 909 MB RSS
  searching 100k chunks of 1024-dimensional vectors.
- **One writer.** SQLite is built with `SQLITE_THREADSAFE=0`. Indexing runs
  embeddings in parallel, but only the main thread writes, and two processes
  must not index the same file at once.

## Retrieval quality

- **Hybrid does not beat cosine with a good embedder.** On SciFact, NFCorpus
  and SQuAD-it (1,123 queries), the paired difference hybrid − cosine has an
  interval straddling zero (p 0.32–0.52). Hybrid is insurance against a weak
  embedder: an English model on Italian text collapses cosine to 0.556 and
  hybrid recovers +0.18. There, BM25 alone (0.803) does better still
  ([`docs/eval-significance.md`](docs/eval-significance.md)).
- **The BM25 is SQLite FTS5's, untuned:**
  - default parameters, `unicode61` tokenizer, no stemming, no stop words;
  - "tumore" and "tumori" do not match lexically;
  - on SciFact it scores nDCG@10 0.654, against 0.665 published for
    Anserini with stemming.
- **The evaluation has blind spots:**
  - relevance is binary;
  - SQuAD-it has one relevant paragraph per question and is a
    machine-assisted translation;
  - there is no graded, hand-labelled set in Italian.
- **A score is not an answer.** The cosine gate (`CRAG_GEN_MIN_SCORE`, and
  `crag-mcp`'s relevance cutoff) drops far-off-topic matches only. A
  same-domain question whose fact is absent still gets plausible chunks:
  same-domain unrelated queries scored cosine 0.45–0.60 in the measurements
  behind the cutoff. The caller must check whether the fact is there.
- **An index belongs to its embedder and to the hardware that computed it.**
  Re-embedding the same corpus on CPU instead of GPU gave vectors at cosine
  0.99998 to the originals, enough to swap 3 of 45 near-tied rankings (seen
  in [askdocs](https://github.com/Simon-oid/askdocs)). Compare results only
  on the same index.

## Inputs

- **Text and Markdown only.** No PDF, HTML or office formats.
- **Chunking is heuristic:** headers, code fences, then a sliding window
  that prefers sentence boundaries. Tables and lists can be split mid-way.
- **The embedder's context window bounds a chunk.** Oversized chunks are
  split again (`CRAG_ECONTEXT`) rather than truncated, but the split points
  are not semantic.

## Platforms and packaging

- **Linux and macOS.** CI builds and tests both. Windows is not supported:
  the indexer uses POSIX `nftw` and pthreads.
- **The Python binding `crag-index` is not on PyPI.** It installs from a
  checkout (`pip install .`). CI builds the wheel and tests it in a clean
  virtualenv on Linux and macOS.
- **End-to-end use needs an embedder server** (Ollama, llama.cpp, or an API
  key). The binding has none: the caller brings the vectors.
