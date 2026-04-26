# cRAG

A from-scratch retrieval-augmented generation pipeline in pure C11.
Index a directory of `.md` / `.txt` files, embed the chunks through
a pluggable backend, store everything in SQLite, then query with
cosine, BM25, or hybrid retrieval — optionally feeding the top-K
into a generator (Claude, GPT, or a local llama.cpp / Ollama).

No C++. No external JSON library. SQLite is vendored. The only
runtime dependencies are libcurl, libm, libpthread, libdl.

```
~2 300 lines of C, two binaries (crag + tests), single Makefile.
```

## Why

Most "RAG in 100 lines" demos lean on Python and a vector DB. This
is an attempt to do the whole thing closer to the metal: paragraph-
and header-aware chunking, a hand-parsed JSON layer, FTS5 for
lexical retrieval, a pthread pool around the embedder, and an
incremental indexer that survives interrupts. Built as a portfolio
piece, intended to be wrapped behind an MCP server next.

## Build & test

```bash
make            # produces ./crag
make test       # builds and runs all tests
make valgrind   # same, under valgrind (optional)
make clean
```

System dependency: `libcurl` headers (`pkg-config --cflags libcurl`
must work). Everything else is vendored or system-standard.

## Five-minute quickstart

The default embedder is local-only (talks to llama.cpp's
`/embedding` endpoint at `localhost:8080`). For an easier on-ramp,
this guide uses Ollama.

```bash
# 1. start the embedder
ollama pull mxbai-embed-large
export CRAG_EMBED_BACKEND=ollama
export CRAG_EMBED_MODEL=mxbai-embed-large

# 2. index a directory
./crag index ./my-docs --db /tmp/notes.db --workers 4

# 3. retrieve
./crag query "how does the foo subsystem retry" --db /tmp/notes.db -k 5

# 4. generate (optional — needs a generator key)
export CRAG_GEN_BACKEND=anthropic
export ANTHROPIC_API_KEY=sk-ant-...
./crag query "summarise the foo retry policy" \
    --db /tmp/notes.db -k 5 --generate
```

`./crag stats --db /tmp/notes.db` prints chunk count, file count, and
the embedding dimension stored in the DB.

## Backends

Plug-in vtables in `src/embed*.c` and `src/gen_*.c`. Pick with
`CRAG_EMBED_BACKEND` / `CRAG_GEN_BACKEND` or the `--backend` flag.

|              | embed | generate | requires |
|---|---|---|---|
| `local`      | ✓ (llama.cpp `/embedding`) | ✓ | `llama-server` running locally |
| `ollama`     | ✓ | ✓ | `ollama serve` running locally |
| `openai`     | ✓ (`text-embedding-3-small`) | ✓ | `OPENAI_API_KEY` |
| `anthropic`  | — | ✓ (default `claude-sonnet-4-6`) | `ANTHROPIC_API_KEY` |

OpenAI's batch endpoint is used natively (up to 256 items per call).
For Ollama and llama.cpp, `--workers N` enables an in-process
pthread pool that fans out per-chunk embedding calls — see below.

## Retrieval

Three search modes, selected with `--search`:

- `cosine` — brute-force scan over all vectors (default before
  hybrid existed; useful baseline).
- `bm25` — FTS5 BM25 over `chunks.text` via an external-content
  virtual table. Skips the embedder entirely.
- `hybrid` (default) — Reciprocal Rank Fusion with k=60 over the
  cosine and BM25 result pools. Robust to embedder weakness on
  rare-keyword queries.

Confidence-gated generation: with cosine search, if the top-1 score
is below `CRAG_GEN_MIN_SCORE` (default 0.60) the generator is not
called and the raw chunks are printed instead.

## Indexing

- **Markdown-aware.** Chunks split on `#` headers; code fences
  protect their contents from the splitter. Short multi-section
  files coalesce into one chunk; long sections fall through to a
  sliding-window split with sentence-boundary preference.
- **Incremental.** A `files` table keyed on `(path, mtime, size)`
  drives skip-unchanged on rerun. Files removed from disk are swept
  from the DB. `--force` bypasses the skip path.
- **Interrupt-safe.** Periodic commits every 20 files bound the
  blast radius of a kill or crash. A `CRAG_ECONTEXT` recursive
  split recovers chunks that exceed the embedder's context window;
  splits are funneled back through the pool so they get
  parallelised too.
- **Parallel.** `--workers N` (default 1) starts a persistent
  pthread pool around `embed_one`. Engaged only when the backend
  has no native batch API. SQLite stays single-threaded
  (`SQLITE_THREADSAFE=0`) — only the main thread inserts.
- **Extension-less docs.** `--ext` filters by extension by default.
  Add `--include-known-docs` to also pick up well-known
  extension-less doc filenames (`README`, `FAQ`, `INSTALL`, `NEWS`,
  `HACKING`, `AUTHORS`, `COPYING`, `LICENSE`, `NOTES`, `THANKS`,
  `TODO`, `ChangeLog`/`CHANGELOG`, `HISTORY`). Useful for distro doc
  trees like `/usr/share/doc/gnupg`.

## Numbers

Measured on `/usr/share/doc` (448 `.md`/`.txt` files, ~10.9 MB) with
Ollama + `mxbai-embed-large` (1024-dim) on a Ryzen 5800X / RX 7800
XT 16 GB:

| | value |
|---|---|
| Cold rebuild (`--workers 4`) | 11 m 27 s (1.64× over 1 worker) |
| Incremental rerun (no changes) | 36 ms |
| Chunks stored | 56 997 |
| Query latency p50 — cosine / hybrid / bm25 | 131 / 133 / **19** ms |
| Spot-check recall@1 (hybrid) | 9 / 10 (9/9 fully-indexed) |
| DB on disk | 278 MB |

Past 4 concurrent embedding calls Ollama serializes hard at the
GPU level — the pool is correct, the bottleneck moves to Ollama.
BM25 is 7× faster because it skips the embedder round-trip.

Full breakdown, prior runs, and the per-query spot-check are in
[`docs/benchmarks.md`](docs/benchmarks.md).

## Layout

```
src/        chunker, store, search, embed/gen backends, indexer, query
include/    public headers
tests/      unit + integration tests; tests/manual/ holds a 20-query golden set
third_party/sqlite3.{c,h}  vendored amalgamation, FTS5 enabled
docs/       benchmarks.md (and a local-only design study)
```

Architecture and design constraints live in
[`CLAUDE.md`](CLAUDE.md).

## Status

cRAG itself is feature-complete for v0.1.0. Phase 2 will wrap it
behind an MCP server so an agent can call `crag_index`, `crag_query`,
`crag_stats` over stdio JSON-RPC. Open items (HNSW, PDF/HTML, the
extension-less doc filename gap, snippet highlighting) are listed
in [`TODO.md`](TODO.md).

## License

MIT. See [`LICENSE`](LICENSE).
