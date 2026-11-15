# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- HTML input (`--ext html`): `src/html.c` converts it in-process, no new
  dependency. Scripts, styles and comments dropped, entities decoded to
  UTF-8 (invalid code points to U+FFFD), headings kept as Markdown so the
  Markdown chunker splits on them. Unit tests, fuzzed in `test_fuzz` with
  an HTML-fragment generator, and covered by `test-cli`.
- `make test-cli` (`tests/test_cli.py`): the CLI end to end against fake
  llama.cpp, Ollama, OpenAI and Anthropic servers on localhost: index,
  query in the three modes, `--generate` through three backends,
  export/import, `--workers`, `--ext`, the confidence gate. Part of
  `make check`, also under ASan/UBSan, and of the macOS job.
- `make coverage` and a CI job: line coverage of `src/` with an 80% floor.
  Measured 81.6% (39.0% before `test-cli`: the index and query paths and
  every backend had no test).

### Changed

- `CRAG_FTS_TOKENIZER` picks the FTS5 tokenizer: `unicode61` (default),
  `porter`, `trigram`, or `italian` (new, `src/fts_italian.c`: unicode61
  plus Savoy's light Italian stemmer). Changing it rebuilds the FTS index
  from `chunks.text` on open, without re-embedding; unset, a database keeps
  its tokenizer; an unknown name refuses to open. Measured, paired:
  SciFact BM25 0.654 → 0.684 with `porter` (+0.030 [+0.010, +0.050]),
  hybrid 0.733 → 0.756; SQuAD-it BM25 0.803 → 0.823 with `italian`
  (+0.020 [+0.004, +0.037]).

- **Exact cosine search 8–9× faster**, level with FAISS `IndexFlatIP`: at
  100k × 1024, 112 → 11.9 ms P50 (FAISS 10.5, NumPy 15.3), same top-10 as
  NumPy for every query (`docs/bench-vs-numpy-faiss.md`). Three changes:
  - an in-memory copy of the vectors and their norms (`crag_vec_cache_t`),
    built at the first search and rebuilt after a write by this connection
    or another one (`PRAGMA data_version`); capped by `CRAG_VEC_CACHE_MB`
    (default 2048, 0 disables). It costs memory: 1.3 GB peak RSS at 100k,
    0.91 before;
  - an 8-lane dot product (`search_dot`, `search_dot_norm`) whose
    summation order is fixed in C, auto-vectorised with an AVX2 clone on
    x86-64 Linux: the same bits on every CPU, and bit-identical to the
    streaming path (tested);
  - the scan's top-k keeps (score, id) and loads text and vector for the
    winners only, instead of copying 16 KB result structs per swap.
- `make bench-scan` (`bench/bench_scan.c`): scan latency against a
  double-precision brute force, without Python.

- The Python binding `crag-index` is versioned with cRAG: 0.2.0, not 1.0.0.
  1.0.0 promised an API stability the project does not yet claim. The
  package was never published to PyPI, so nothing depends on the old
  number.

### Fixed

- A deleted file stayed in the index when `crag index` ran again within
  the same second as the previous run: the sweep removed files with
  `indexed_at < run_ts`, at one-second resolution. A run's stamp is now
  past the latest stored one (`store_next_run_ts`). Found by `test-cli`.
- The `openai` embedding backend ignored `CRAG_EMBED_URL` and always called
  `api.openai.com`, so an OpenAI-compatible server could only be set with
  `--url`. It now reads the variable, like the other backends.

- The Makefile now tracks header dependencies (`-MMD -MP`): a change to
  `include/*.h` rebuilds the objects that include it. Before, `make` after
  bumping `CRAG_VERSION` left a stale `main.o` and `crag --version` still
  printed the old version.

## [0.2.0] — 2026-11-11

### Added

- **Library API** `include/crag_lib.h` (`src/lib.c`): open / add / begin /
  commit / search (cosine, bm25, hybrid) / hits_free / stats / close over the
  index, with documented ownership and one index-wide vector dim.
  `crag_errstr` moved from `main.c` to `src/errstr.c` so the library stands
  alone. Tested by `tests/test_lib.c`.
- **Python binding** `crag-index` (`python/`, `pyproject.toml`, `setup.py`):
  cffi API mode, a self-contained wheel (index + vendored SQLite, no curl),
  zero-copy float32 vectors via `ffi.from_buffer`, type errors instead of
  silent conversion. CI job `python-wheel` builds it on Linux and macOS and
  tests it in a clean virtualenv, with and without NumPy.
- **Evaluation with uncertainty**: `eval_beir --per-query` writes per-query
  scores; `tests/eval/significance.py` reports 95% bootstrap intervals and a
  paired permutation test; the report is versioned in
  `docs/eval-significance.md` and the per-query CSVs in `tests/eval/`.
- **Italian benchmark** `DATASET=squadit`: 2,010 SQuAD-it paragraphs and 500
  seeded questions in BEIR layout (`tests/eval/fetch_squadit.py`). It
  replaces the planned hand-labelled qrels on the Smart City fixture, which
  is 20 templates duplicated ~5× and has no relevance gradient.

- **Speed benchmark** `bench/vs_numpy_faiss.py`: exact top-10 cosine vs NumPy
  and FAISS IndexFlatIP at 10k and 100k chunks, per-engine process, P50/P95,
  build time, peak RSS and a same-results check (`docs/bench-vs-numpy-faiss.md`).

### Changed

- README states exactly what the BM25 is (SQLite FTS5 `bm25()`, k1 1.2, b
  0.75, `unicode61`, no stemming) and reports the measured answer to "when
  does hybrid beat cosine": with a suitable embedder it does not
  (differences not significant on SciFact, NFCorpus, SQuAD-it); with an
  unsuitable one it recovers +0.18 nDCG@10.

## [0.1.0] — 2026-06-12

First feature-complete release: a from-scratch RAG pipeline in pure C11.

### Added

**Pipeline**
- Markdown-aware chunker (`src/chunk.c`): header/code-fence handling, section
  coalescing, sliding-window fallback with sentence-boundary preference and a
  no-progress guard.
- Pluggable embedding backends via the `embed_backend_t` vtable: `local`
  (llama.cpp `/embedding`), `ollama`, `openai` (`text-embedding-3-small`,
  batched up to 256/call).
- SQLite vector store (`src/store.c`): chunk BLOBs + an FTS5 shadow table, a
  `files` table for incremental reindex, WAL mode, `SQLITE_THREADSAFE=0`,
  vendored amalgamation.
- Three retrieval modes: cosine (exact scan), BM25 (FTS5), and hybrid
  (Reciprocal Rank Fusion, k=60, default).
- Pluggable generation backends via vtable: `anthropic` (default
  `claude-sonnet-4-6`), `openai`, `local`/`ollama`.

**Indexing**
- Parallel indexing with an in-process pthread pool around `embed_one`
  (`--workers N`), engaged only for non-batch backends; SQLite stays
  single-threaded.
- Interrupt-safe indexing: periodic commits every 20 files, `CRAG_ECONTEXT`
  recursive split fan-out, and per-file drop diagnostics.
- Incremental reindex keyed on `(path, mtime, size)` with sweep-on-missing;
  `--force` for a full rebuild (also as `crag reindex`).
- `--include-known-docs` to index extension-less well-known doc filenames.

**Query & generation**
- Embedder-aware confidence gate for `--generate` (`bge-m3` → 0.38, else 0.60).
- Query-time snippet highlighting (ANSI on a TTY, `<<term>>` when piped).
- `CRAG_GEN_K` to cap the generation context to the top-N chunks (opt-in).
- Deterministic generation by default (`CRAG_GEN_TEMP=0`).

**Tooling**
- Bulk `export` / `import` as embedder-portable JSONL.
- `crag stats` (chunk/file count + stored embedding dimension).

**Quality & robustness**
- `make check` gate: full suite under valgrind, ASan/UBSan, and TSan.
- Adversarial parser fuzzer (`tests/test_fuzz.c`) — caught and fixed a real heap
  overflow in the rerank response parser.
- Structural prompt-injection tests (`tests/test_injection.c`).
- Determinism test (`tests/test_determinism.c`).
- Offline retrieval-quality regression gate (`make eval-ci`): deterministic,
  server-free nDCG/Recall/MRR floors.
- BEIR evaluation harness (`make eval`, SciFact / NFCorpus) with Python
  reference implementations.
- GitHub Actions CI running the full `make check` gate on every push/PR, plus
  a macOS `make test` job backing the Linux/macOS portability claim.

**Documentation**
- Public-facing documentation: a polished `README.md` (with a terminal demo
  GIF), `ARCHITECTURE.md` (design deep-dive + full CLI reference),
  `ROADMAP.md`, a `docs/` index, and issue/PR templates.
- MCP integration documented: the [`crag-mcp`](https://github.com/Simon-oid/cMCP/tree/main/tools/crag-mcp)
  reference server (in the sibling cMCP project) exposes the index as
  `crag_search` / `crag_stats` tools; agent-in-the-loop behaviour validated in
  `docs/agent-validation.md`.

### Measured
- BEIR SciFact: exact cosine scores nDCG@10 **0.7407** with `mxbai-embed-large`,
  verified bit-for-bit against NumPy and FAISS `IndexFlatIP` — zero retrieval
  loss over the embedder.
- Cross-encoder reranking measured to **not** help, across SciFact and NFCorpus;
  left wired but off by default.
- Embedder language probe: `mxbai-embed-large` 8/20 vs `bge-m3` 20/20 on Italian
  paraphrase queries → `bge-m3` documented as the non-English default.

[Unreleased]: https://github.com/Simon-oid/cRAG/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/Simon-oid/cRAG/releases/tag/v0.2.0
[0.1.0]: https://github.com/Simon-oid/cRAG/releases/tag/v0.1.0
