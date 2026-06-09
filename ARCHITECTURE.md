# Architecture

This document describes how cRAG is built: the module boundaries, the data
flow through the pipeline, the design constraints that shaped it, and a full
CLI reference. For *why* certain features exist (or were deliberately left
out), see [`ROADMAP.md`](ROADMAP.md). For measured numbers, see
[`docs/benchmarks.md`](docs/benchmarks.md).

## Design goals

cRAG is a from-scratch RAG engine with a small, explicit dependency surface:

- **Pure C11.** No C++, no external JSON library — JSON is hand-parsed in
  `src/util.c`. The only runtime dependencies are `libcurl`, `libm`,
  `libpthread`, and `libdl`. SQLite is vendored as an amalgamation in
  `third_party/`.
- **Exact and deterministic.** Cosine search is a brute-force scan with no
  approximation. Given the same corpus and query, results are byte-identical
  across runs (pinned by `tests/test_determinism.c`).
- **Personal / single-tenant envelope.** The design target is thousands to a
  few hundred thousand chunks on one machine. ANN indexes and vector
  quantization — which trade exactness for scale — are intentionally out of
  scope (see [`ROADMAP.md`](ROADMAP.md)).
- **Pluggable I/O.** Embedding and generation are vtable-based, so adding a
  backend is a single new `.c` file implementing an interface.
- **Agent-ready.** The query result carries enough provenance (per-result
  cosine/BM25/fusion scores + source path + chunk text) for an LLM host to
  cite sources and refuse out-of-context questions. That logic lives in the
  consuming agent/MCP layer, not in the C library.

## The pipeline

The pipeline has five stages, each in its own module.

### 1. Chunking — `src/chunk.c`

Splits files into overlapping text chunks. Markdown-aware: it splits on `#`
headers and preserves code fences (content inside ``` ``` ``` is never split
mid-fence). Configurable chunk size (default 2048 chars) and overlap
(200 chars).

- Short multi-section files **coalesce** consecutive header sections into a
  single chunk until adding the next section would overflow `max_chars`.
- Sections that individually exceed the limit fall through to `sliding_split`,
  a sliding window that prefers sentence boundaries and guarantees forward
  progress (a fix for a no-progress loop on unbreakable content like long
  base64 blobs — see `tests/test_chunker.c`).

### 2. Embedding — `src/embed*.c`

A plugin architecture via the `embed_backend_t` vtable (`embed_one`,
`embed_many`). Three backends:

| File | Backend | Notes |
|---|---|---|
| `src/embed_local.c` | `local` | POSTs to llama.cpp's `/embedding` endpoint (default `http://localhost:8080/embedding`). Parses all three llama.cpp response shapes. |
| `src/embed_ollama.c` | `ollama` | One HTTP POST per chunk; no native batch, so it benefits from `--workers`. |
| `src/embed_api.c` | `openai` | `text-embedding-3-small`; batches up to 256 items per call via `embed_many`. |

`src/embed.c` selects the backend from `CRAG_EMBED_BACKEND` / `--backend`.
`src/embed_pool.c` is the pthread worker pool used when a backend has no native
batch path.

### 3. Store — `src/store.c`

A SQLite-backed vector database:

- `chunks` — chunk text + the embedding stored as a BLOB, with the source path.
- `chunks_fts` — an FTS5 virtual table shadowing `chunks.text` via triggers,
  for lexical (BM25) search.
- `files` — `(path, mtime, size, indexed_at)`, which drives incremental
  reindex.

All chunks in a single database **must share the same vector dimension**;
mixing embedders in one DB is rejected (`CRAG_EDIM_MISMATCH`).

SQLite is compiled with `SQLITE_THREADSAFE=0`, WAL mode, and
`SQLITE_ENABLE_FTS5`. Because it is single-threaded, **only the main thread
ever calls into the store.**

### 4. Search — `src/search.c`, `src/store.c`

Three modes:

- **cosine** — in-memory insertion sort after a full vector scan. Exact.
- **bm25** — FTS5 BM25. Skips the embedder entirely.
- **hybrid** (default) — Reciprocal Rank Fusion with `k=60`, blending the
  cosine and BM25 result pools.

### 5. Generation — `src/generate.c`, `src/gen_*.c`

A plugin architecture via vtable. Backends: `anthropic` (Claude, default
`claude-sonnet-4-6`), `openai`, `local`/`ollama`. The generator takes a system
prompt plus a user prompt built from the retrieved chunks and returns an
allocated string the caller owns.

## Data flow

### Indexing

```text
nftw() walk
  └─ store_file_is_current()   ── skip unchanged (unless --force)
       └─ chunk_file()
            └─ embed_many()    ── batched; or the pthread pool for non-batch backends
                 └─ store_insert()   (in a transaction, main thread only)
                      └─ store_commit()   every 20 files
after the walk:
  store_sweep_missing()         ── delete rows for files no longer on disk
```

`--workers N` (default 1) spins up an in-process pthread pool around
`embed_one`, engaged only when the backend has no native batch path (e.g.
Ollama). The pool also handles the recursive `CRAG_ECONTEXT` splits, so split
halves benefit from worker parallelism too. The walk filter `has_ext()` matches
by extension via `--ext`; with `--include-known-docs` it additionally accepts a
whitelist of extension-less basenames.

### Querying

```text
embed_one(query)              ── skipped in bm25 mode
  └─ store_search_{cosine,bm25,hybrid}()
       └─ top-K chunks
            └─ generator_run()   ── optional (--generate)
                 └─ text or JSON output
```

The confidence gate (`CRAG_GEN_MIN_SCORE`) is only active in cosine mode, since
BM25/RRF scores use a different scale. Its default is resolved per embedder by
`gen_min_score_resolve` in `src/util.c` (`bge-m3` → 0.38, else 0.60).

## Key design constraints

- **Pure C11, no C++ or external JSON library.** JSON is hand-parsed in
  `src/util.c`; all HTTP goes through `libcurl` in `src/http.c`.
- **SQLite is single-threaded** (`SQLITE_THREADSAFE=0`). The `--workers N` pool
  only parallelises `embed_one`; inserts stay on the main thread.
- **Manual memory management.** `malloc`/`free` throughout; the caller owns
  returned memory. Negative return values are error codes (`crag_err_t` in
  `include/crag.h`).
- **The `local` backend** talks to llama.cpp's `/embedding` endpoint; override
  with `CRAG_EMBED_URL`.

## Error codes

Defined in `include/crag.h`; rendered by `crag_errstr` in `src/main.c`.

| Code | Meaning |
|---|---|
| `CRAG_OK` | Success. |
| `CRAG_EINVAL` | Invalid argument. |
| `CRAG_ENOMEM` | Out of memory. |
| `CRAG_EIO` | I/O error. |
| `CRAG_EPARSE` | Parse error. |
| `CRAG_EHTTP` | HTTP error. |
| `CRAG_EDIM_MISMATCH` | Embedding dimension mismatch (mixed embedders in one DB). |
| `CRAG_EDB` | Database error. |
| `CRAG_ECONTEXT` | Input exceeds the embedder's context window (triggers a recursive split). |

## CLI reference

Run `crag` with no arguments to print this usage. Flags take precedence over
the corresponding environment variables.

### `crag index <dir>`

Walk `<dir>`, chunk, embed, and store. Incremental by default (unchanged files
are skipped).

| Flag | Meaning |
|---|---|
| `--db <path>` | Database path (default `CRAG_DB` or `./crag.db`). |
| `--ext <list>` | Comma-separated extensions to index (default `txt,md`). |
| `--backend <name>` | `local` \| `openai` \| `ollama`. |
| `--max-bytes <N>` | Skip files larger than `N` bytes. |
| `--url <url>` | Custom embedding endpoint. |
| `--force` | Re-index every file, ignoring the skip-unchanged check. |
| `--workers <N>` | Embedding worker threads (default 1; non-batch backends only). |
| `--include-known-docs` | Also index extension-less well-known doc filenames. |

### `crag reindex <dir>`

Alias for `index --force <dir> [opts...]`. A full rebuild.

### `crag query <text>`

Retrieve the top-K chunks for `<text>`.

| Flag | Meaning |
|---|---|
| `--db <path>` | Database path. |
| `-k <N>` | Number of chunks to return. |
| `--format <fmt>` | `text` (default) or `json`. |
| `--generate` | Synthesise an answer from the retrieved chunks. |
| `--search <mode>` | `cosine` \| `bm25` \| `hybrid` (default). |
| `--backend <name>` | Embedding backend for the query vector. |
| `--url <url>` | Custom embedding endpoint. |

### `crag stats`

Print the database path, chunk count, file count, and stored embedding
dimension. Accepts `--db <path>`.

### `crag export`

Dump every chunk as one JSON object per line
(`{"source":"...","text":"..."}`). Vectors are intentionally omitted so the
dump is portable across embedders. Accepts `--db <path>` and `--out <path>`
(stdout by default).

### `crag import <file>`

Read a JSONL dump, re-embed each row through the configured backend, and
insert. Accepts `--db <path>`, `--backend <name>`, `--url <url>`. The `files`
table is not touched — a later `index` over the same source paths replaces
these rows as usual.

### `crag --version` / `crag -v`

Print the version string.

## Testing

Tests use a minimal macro framework in `tests/test.h` (`TEST_ASSERT`,
`TEST_RUN`, `TEST_DONE`). Tests create temporary files under `/tmp/` and clean
up after themselves; there is no mocking — store tests use real SQLite.

| Binary | Covers |
|---|---|
| `test_chunker` | Markdown-aware chunker, coalescing, no-progress guard. |
| `test_search` | Cosine similarity math. |
| `test_store` | SQLite vector store + top-K. |
| `test_util` | JSON escape / float-array parse / gate resolution. |
| `test_retrieval` | End-to-end: docs → chunk → embed → store → query (toy embedder). |
| `test_rerank` | Cross-encoder rerank response parser (offline). |
| `test_fuzz` | Adversarial fuzzer for the hand-rolled parsers. |
| `test_injection` | Prompt-injection payloads escape to inert JSON data. |
| `test_crash_safe` | Interrupt/crash safety of the indexer. |
| `test_determinism` | Same query is byte-identical across runs. |
| `test_hostile_embed` / `test_hostile_query` | Malformed backend responses and queries. |
| `test_embed_pool` | The pthread worker pool. |
| `eval_ci` | Offline nDCG/Recall/MRR regression floor (toy embedder). |

`make check` runs all of the above under plain, valgrind, ASan/UBSan, and TSan
builds. The fuzzer's teeth come from `make test-asan`, where every scratch
buffer is an ASan-red-zoned heap allocation. Absolute real-world retrieval
quality is measured separately by `make eval` (BEIR; needs a live embedder).
