# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

[0.1.0]: https://github.com/Simon-oid/cRAG/releases/tag/v0.1.0
