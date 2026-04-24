# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
make              # build ./crag
make clean        # remove objects and binaries
make test         # build and run all tests
make valgrind     # run all tests under valgrind (requires valgrind installed)
```

Run a single test binary directly after `make test`:
```bash
./tests/test_chunker     # markdown-aware chunker
./tests/test_search      # cosine similarity math
./tests/test_store       # SQLite vector store + top-K
./tests/test_util        # JSON escape / float-array parse
./tests/test_retrieval   # end-to-end: docs → chunk → embed → store → query
```

`test_retrieval` uses a small in-test hashed bag-of-words embedder to keep the suite pure C and offline; it verifies topic queries retrieve the correct document.

Dependencies: `libcurl`, `libm`, `libpthread`, `libdl`. SQLite3 is vendored in `third_party/` and compiled with `SQLITE_THREADSAFE=0` and WAL mode.

## Architecture

The pipeline has five stages, each in its own module:

1. **Chunking** (`src/chunk.c`) — Splits files into overlapping text chunks. Markdown-aware: splits on headers, preserves code fences. Configurable chunk size (default 2048 chars) and overlap (200 chars).

2. **Embedding** (`src/embed*.c`) — Plugin architecture via `embed_backend_t` vtable (`embed_one`, `embed_many`). Three backends: `openai` (text-embedding-3-small), `ollama`, `local` (stub). Batches up to 256 items per API call.

3. **Store** (`src/store.c`) — SQLite-backed vector database. Chunks stored as BLOBs with source path and text. All chunks in a single database must share the same vector dimension (enforced at search time).

4. **Search** (`src/search.c`) — Cosine similarity only. Top-K selection via in-memory insertion sort after a full table scan.

5. **Generation** (`src/generate.c`, `src/gen_*.c`) — Plugin architecture via vtable. Backends: `anthropic` (Claude, default `claude-sonnet-4-6`), `openai`, `local`. Accepts system prompt + user prompt, returns allocated string.

**Index data flow:** `nftw()` walk → `chunk_file()` → `embed_many()` (batched) → `store_insert()` in transaction → `store_commit()`

**Query data flow:** `embed_one(query)` → `store_search()` cosine scan → top-K chunks → optionally `generator_run()` → text or JSON output

## Key Design Constraints

- Pure C11, no C++ or external JSON libraries. JSON is hand-parsed in `src/util.c`.
- All HTTP via `libcurl` (`src/http.c`).
- Single-threaded — `SQLITE_THREADSAFE=0`; do not add concurrent store access.
- Manual `malloc`/`free` throughout; caller owns returned memory.
- The `local` embedding backend is a stub (returns zeros); real implementation needed.
- Negative return values are error codes (`crag_err_t` in `include/crag.h`).

## Environment Variables

| Variable | Purpose |
|---|---|
| `CRAG_DB` | Default database path |
| `CRAG_EMBED_BACKEND` | `local` (default), `openai`, `ollama` |
| `CRAG_EMBED_URL` | Custom embedding API endpoint |
| `OPENAI_API_KEY` | Required for OpenAI embedding backend |
| `CRAG_GEN_BACKEND` | `local` (default), `anthropic`, `openai` |
| `CRAG_GEN_URL` | Custom generation API endpoint |
| `CRAG_GEN_MODEL` | Model override (e.g., `claude-sonnet-4-6`) |
| `CRAG_GEN_MAX_TOKENS` | Max tokens in generated response (default: 1024) |
| `ANTHROPIC_API_KEY` | Required for Anthropic generation backend |

## Test Framework

Tests use a minimal macro framework in `tests/test.h` (`TEST_ASSERT`, `TEST_RUN`, `TEST_DONE`). Tests create temporary files under `/tmp/` and clean up after themselves. No mocking — store tests use real SQLite.
