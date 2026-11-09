# cRAG

[![CI](https://github.com/Simon-oid/cRAG/actions/workflows/ci.yml/badge.svg)](https://github.com/Simon-oid/cRAG/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Language: C11](https://img.shields.io/badge/language-C11-00599C.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Dependencies: minimal](https://img.shields.io/badge/deps-libcurl%20%C2%B7%20libm%20%C2%B7%20pthread-lightgrey.svg)](#requirements)

**A from-scratch retrieval-augmented generation pipeline in pure C11.**

Index a directory of `.md` / `.txt` files, embed the chunks through a
pluggable backend, store everything in SQLite, then query with cosine,
BM25, or hybrid retrieval — optionally feeding the top-K into a generator
(Claude, GPT, or a local llama.cpp / Ollama).

No C++. No external JSON library. SQLite is vendored. The only runtime
dependencies are `libcurl`, `libm`, `libpthread`, `libdl`.

```text
~3 300 lines of C  ·  one binary (crag) + a test suite  ·  single Makefile
```

![cRAG demo: index, incremental rerun, hybrid query with term highlighting, local generation](docs/demo.gif)

---

## Table of contents

- [Why](#why)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Build &amp; test](#build--test)
- [Five-minute quickstart](#five-minute-quickstart)
- [Commands](#commands)
- [Backends](#backends)
- [Retrieval](#retrieval)
- [Indexing](#indexing)
- [Configuration](#configuration)
- [Benchmarks](#benchmarks)
- [MCP / agent integration](#mcp--agent-integration)
- [Project layout](#project-layout)
- [Documentation](#documentation)
- [Status &amp; roadmap](#status--roadmap)
- [License](#license)

---

## Why

Most "RAG in 100 lines" demos lean on Python and a managed vector DB. This
is an attempt to do the whole thing closer to the metal: paragraph- and
header-aware chunking, a hand-parsed JSON layer, FTS5 for lexical retrieval,
a pthread pool around the embedder, and an incremental indexer that survives
interrupts. Built as a portfolio piece, hardened with sanitizer/fuzzer gates,
and designed to be driven by an LLM agent through an
[MCP wrapper](#mcp--agent-integration) — which exists and is validated.

It deliberately targets the **personal / single-tenant** operating envelope
(thousands to a few hundred thousand chunks): exact brute-force cosine, a
single SQLite file, byte-identical deterministic results. ANN indexes and
vector quantization — the enterprise scale story — are out of scope by
design. See [`ROADMAP.md`](ROADMAP.md) for the reasoning.

## How it works

The pipeline has five stages, each in its own module:

```text
                 ┌──────────┐   ┌───────────┐   ┌──────────┐
   files  ──▶    │  chunk   │──▶│   embed   │──▶│  store   │   SQLite
 (.md/.txt)      │ markdown │   │ (backend) │   │  vectors │   + FTS5
                 │  aware   │   │  batched  │   │  + text  │
                 └──────────┘   └───────────┘   └────┬─────┘
                                                     │
                              query  ──▶  embed  ──▶ │
                                                ┌────▼─────┐   ┌──────────┐
                                                │  search  │──▶│ generate │──▶ answer
                                                │ cosine / │   │ (backend)│    (optional)
                                                │ bm25 /   │   │  Claude  │
                                                │ hybrid   │   │  GPT …   │
                                                └──────────┘   └──────────┘
```

1. **Chunking** — markdown-aware splitter (headers + code fences), sliding
   window with sentence-boundary preference for long sections.
2. **Embedding** — pluggable `embed_backend_t` vtable: `local` (llama.cpp),
   `ollama`, `openai`. Batched up to 256 items per call where the API allows.
3. **Store** — SQLite: chunk BLOBs + an FTS5 shadow table for lexical search,
   plus a `files` table that drives incremental reindex.
4. **Search** — cosine (exact scan), BM25 (FTS5), or hybrid (Reciprocal Rank
   Fusion, k=60). Hybrid is the default.
5. **Generation** — pluggable vtable: `anthropic` (default
   `claude-sonnet-4-6`), `openai`, `local`/`ollama`.

A full design walkthrough lives in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Requirements

- A C11 compiler (`gcc` or `clang`).
- `libcurl` development headers — `pkg-config --cflags libcurl` must work.
- `libm`, `libpthread`, `libdl` (system-standard on Linux/macOS).
- Optional, for end-to-end use: a running [Ollama](https://ollama.com) or
  [llama.cpp](https://github.com/ggerganov/llama.cpp) server, or an
  `OPENAI_API_KEY` / `ANTHROPIC_API_KEY`.

SQLite is **vendored** in `third_party/` (amalgamation, FTS5 enabled,
`SQLITE_THREADSAFE=0`) — nothing to install.

## Build &amp; test

```bash
make            # produces ./crag
make test       # builds and runs the full test suite (incl. the eval-ci gate)
make check      # full robustness gate: tests + valgrind + ASan/UBSan + TSan
make valgrind   # tests under valgrind only
make clean      # remove objects and binaries
```

`make check` is the fail-fast robustness gate that CI runs on every push: plain
tests → valgrind → ASan/UBSan → TSan, plus the offline retrieval-quality
regression floor (`make eval-ci`). It needs **no embedder, no network, no
secrets** — a deterministic toy embedder and a checked-in synthetic corpus
stand in.

## Five-minute quickstart

The default embedder is local-only (talks to llama.cpp's `/embedding` endpoint
at `localhost:8080`). For an easier on-ramp, this guide uses Ollama.

```bash
# 1. start the embedder
ollama pull mxbai-embed-large       # English; for Italian/multilingual use bge-m3
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

> **Choosing an embedder — language matters.** `mxbai-embed-large` is
> English-centric. For non-English corpora (e.g. Italian municipal docs) prefer
> a multilingual model — the recommended default is **`bge-m3`**
> (`ollama pull bge-m3`; also 1024-dim, so it is a drop-in). On a 20-query
> Italian paraphrase probe (synonyms, low lexical overlap), `mxbai-embed-large`
> matched only **8/20** topics at rank 1 versus **20/20 for `bge-m3`** (see
> [`docs/benchmarks.md`](docs/benchmarks.md)). All chunks in one DB share an
> embedder and dimension, so choose before indexing.

## Commands

| Command | Purpose |
|---|---|
| `crag index <dir>` | Walk a directory, chunk, embed, and store. Incremental by default. |
| `crag reindex <dir>` | Sugar for `index --force` — full rebuild. |
| `crag query <text>` | Retrieve top-K chunks; `--generate` to also synthesise an answer. |
| `crag stats` | Print chunk count, file count, and the stored embedding dimension. |
| `crag export` | Dump every chunk as JSONL (`{"source","text"}`), vectors omitted. |
| `crag import <file>` | Read a JSONL dump back, re-embed, and insert. |
| `crag --version` | Print version. |

`export`/`import` are embedder-portable: the dump carries text only, so you can
re-embed an existing corpus through a different backend without re-reading the
source files. Full flag reference: run `crag` with no arguments, or see
[`ARCHITECTURE.md`](ARCHITECTURE.md).

## Backends

Plug-in vtables in `src/embed*.c` and `src/gen_*.c`. Pick with
`CRAG_EMBED_BACKEND` / `CRAG_GEN_BACKEND` or the `--backend` flag.

| | embed | generate | requires |
|---|---|---|---|
| `local`      | ✓ (llama.cpp `/embedding`) | ✓ | `llama-server` running locally |
| `ollama`     | ✓ | ✓ | `ollama serve` running locally |
| `openai`     | ✓ (`text-embedding-3-small`) | ✓ | `OPENAI_API_KEY` |
| `anthropic`  | — | ✓ (default `claude-sonnet-4-6`) | `ANTHROPIC_API_KEY` |

OpenAI's batch endpoint is used natively (up to 256 items per call). For Ollama
and llama.cpp, `--workers N` enables an in-process pthread pool that fans out
per-chunk embedding calls — see [Indexing](#indexing).

## Retrieval

Three search modes, selected with `--search`:

- **`cosine`** — exact brute-force scan over all vectors. Deterministic and
  byte-identical; the quality baseline.
- **`bm25`** — FTS5 BM25 over `chunks.text` via an external-content virtual
  table. Skips the embedder entirely (≈7× faster).

  **What this BM25 is, exactly.** It is SQLite FTS5's built-in `bm25()`
  ranking function, not an implementation of cRAG's own: default parameters
  (k1 = 1.2, b = 0.75), FTS5's default `unicode61` tokenizer (Unicode-aware
  case folding and diacritic removal, **no stemming and no stop-word list**,
  in any language), one indexed column per chunk. That choice buys a mature,
  crash-safe inverted index inside the same SQLite file as the vectors, at
  the cost of tuning: a dedicated IR toolkit with stemming does a little
  better, e.g. on BEIR SciFact FTS5 scores nDCG@10 0.6540 against Anserini's
  published 0.665. No stemming also means Italian inflections ("tumore" /
  "tumori") do not match each other lexically; the dense side of `hybrid`
  covers that.
- **`hybrid`** (default) — Reciprocal Rank Fusion (k=60) over the cosine and
  BM25 result pools. Robust to embedder weakness on rare-keyword queries.

**Confidence-gated generation:** with cosine search, if the top-1 score is
below `CRAG_GEN_MIN_SCORE` the generator is not called and the raw chunks are
printed instead. The default is embedder-aware (`bge-m3` → 0.38, else 0.60).

**Snippet highlighting:** in `bm25`/`hybrid` modes, matched terms in printed
chunks are wrapped — ANSI bold-yellow on a TTY, literal `<<term>>` markers when
piped or redirected. JSON output stays unmarked; cosine output is unchanged.

## Indexing

- **Markdown-aware.** Chunks split on `#` headers; code fences protect their
  contents from the splitter. Short multi-section files coalesce into one chunk;
  long sections fall through to a sliding-window split with sentence-boundary
  preference.
- **Incremental.** A `files` table keyed on `(path, mtime, size)` drives
  skip-unchanged on rerun. Files removed from disk are swept from the DB.
  `--force` bypasses the skip path.
- **Interrupt-safe.** Periodic commits every 20 files bound the blast radius of
  a kill or crash. A `CRAG_ECONTEXT` recursive split recovers chunks that exceed
  the embedder's context window; splits are funneled back through the pool so
  they get parallelised too.
- **Parallel.** `--workers N` (default 1) starts a persistent pthread pool
  around `embed_one`. Engaged only when the backend has no native batch API.
  SQLite stays single-threaded (`SQLITE_THREADSAFE=0`) — only the main thread
  inserts.
- **Extension-less docs.** `--ext` filters by extension by default. Add
  `--include-known-docs` to also pick up well-known extension-less doc
  filenames (`README`, `FAQ`, `INSTALL`, `NEWS`, `HACKING`, `AUTHORS`,
  `COPYING`, `LICENSE`, `NOTES`, `THANKS`, `TODO`, `ChangeLog`/`CHANGELOG`,
  `HISTORY`). Useful for distro doc trees like `/usr/share/doc/gnupg`.

## Configuration

All configuration is via environment variables (or flags, which take
precedence).

| Variable | Purpose |
|---|---|
| `CRAG_DB` | Default database path. |
| `CRAG_EMBED_BACKEND` | `local` (default), `openai`, `ollama`. |
| `CRAG_EMBED_MODEL` | Embedding model. English: `mxbai-embed-large`. Non-English: **`bge-m3`** (multilingual, 1024-dim drop-in). |
| `CRAG_EMBED_URL` | Custom embedding API endpoint. |
| `OPENAI_API_KEY` | Required for the OpenAI embedding backend. |
| `CRAG_GEN_BACKEND` | `local` (default), `anthropic`, `openai`, `ollama`. |
| `CRAG_GEN_URL` | Custom generation endpoint. |
| `CRAG_GEN_MODEL` | Model override (e.g. `claude-sonnet-4-6`). |
| `CRAG_GEN_MAX_TOKENS` | Max tokens in the generated response (default 1024). |
| `CRAG_GEN_MIN_SCORE` | Confidence gate for `--generate` (cosine mode). Overrides the embedder-aware default (`bge-m3` → 0.38, else 0.60). |
| `CRAG_GEN_TEMP` | Generation sampling temperature (ollama). Default `0` (deterministic). |
| `CRAG_GEN_K` | Cap the generation context to the top-N retrieved chunks (default: unset = feed all `--top-k`). `2` is the measured floor that helps larger gen models. |
| `ANTHROPIC_API_KEY` | Required for the Anthropic generation backend. |

## Benchmarks

Measured on `/usr/share/doc` (627 `.md`/`.txt`/known-doc files, ~12 MB) with
Ollama + `mxbai-embed-large` (1024-dim) on a Ryzen 5800X / RX 7800 XT 16 GB:

| | value |
|---|---|
| Cold rebuild (`--workers 4 --include-known-docs`) | 5 m 19 s |
| Incremental rerun (no changes) | ~40 ms |
| Chunks stored | 13 541 |
| Query latency p50 — cosine / hybrid / bm25 | 56 / 62 / **16** ms |
| Spot-check recall@1 (hybrid) | **10 / 10** |
| DB on disk | 82 MB |

On **BEIR SciFact**, cRAG's exact cosine scores nDCG@10 **0.7407** with
`mxbai-embed-large` — in the published dense band, and verified **bit-for-bit
against NumPy and FAISS `IndexFlatIP`** on identical vectors, so the engine adds
zero retrieval loss over its embedder. The one true engine-vs-engine comparison
is BM25, where cRAG's FTS5 scores **0.6540** vs Anserini's 0.665 (~1 point — the
honest gap between an embedded FTS5 and a tuned IR toolkit). Full breakdown, the
cross-embedder sweep, NFCorpus, and the per-query spot-check are in
[`docs/benchmarks.md`](docs/benchmarks.md) and the
[performance report](docs/crag-performance-report.pdf).

### When does hybrid beat vector-only search? (measured 2026-11-09)

nDCG@10 with a 95% bootstrap interval, and the paired difference hybrid −
cosine with a paired permutation test (10,000 rounds each, fixed seed).
Ollama 0.40.4 embedders on a Ryzen 7 5800X; every number below is printed by
`tests/eval/run.sh` + `tests/eval/significance.py` from the per-query CSVs in
`tests/eval/` ([full report](docs/eval-significance.md)).

| Dataset (queries) | Embedder | cosine | bm25 | hybrid | hybrid − cosine | p |
|---|---|---|---|---|---|---|
| SciFact, en (300) | mxbai-embed-large | 0.742 [0.699, 0.782] | 0.654 [0.609, 0.699] | 0.733 [0.691, 0.774] | −0.009 [−0.036, +0.018] | 0.52 |
| NFCorpus, en (323) | mxbai-embed-large | 0.375 [0.339, 0.411] | 0.300 [0.266, 0.334] | 0.368 [0.334, 0.405] | −0.006 [−0.022, +0.009] | 0.44 |
| SQuAD-it, it (500) | bge-m3 | 0.845 [0.820, 0.869] | 0.804 [0.774, 0.833] | 0.856 [0.832, 0.879] | +0.011 [−0.012, +0.033] | 0.32 |
| SQuAD-it, it (500) | mxbai-embed-large | 0.556 [0.520, 0.592] | 0.803 [0.773, 0.832] | 0.737 [0.706, 0.767] | **+0.181** [+0.156, +0.207] | <0.001 |

The answer the data gives:

- **With an embedder suited to the language, hybrid and cosine are
  indistinguishable** — on all three datasets the interval of the difference
  straddles zero. Hybrid always beats BM25 alone (p < 0.001).
- **With the wrong embedder, hybrid is the safety net.** An English model on
  Italian text collapses cosine to 0.556; hybrid recovers +0.18 — but here BM25
  alone (0.803) is better still. Hybrid is insurance against a weak embedder,
  not a free gain over a good one.
- Caveats: binary relevance; SQuAD-it has one relevant paragraph per
  question and is a machine-assisted translation (see
  `tests/eval/fetch_squadit.py`), so its absolute numbers are not comparable
  with the English sets — the comparisons within a row are the signal.

## Use it as a library

The index underneath the `crag` program is a stable C API,
[`include/crag_lib.h`](include/crag_lib.h): open an index, add chunks with
your own vectors, search in any of the three modes, free the hits. Every
function documents who owns each pointer; return codes are `crag_err_t`.

The same API is packaged for Python with **cffi** (API mode, so the compiler
checks the binding against the header). The wheel compiles cRAG's index and
the vendored SQLite into the package, so `pip install` needs nothing from the
system but a C compiler, and NumPy `float32` vectors reach C without a copy:

```bash
pip install .            # from a checkout; builds the wheel
```

```python
import numpy as np, crag

with crag.Index("notes.db") as idx:
    idx.add_many((src, text, vec) for src, text, vec in chunks)   # vec: float32
    hits = idx.search(vector=q, text="query words", k=5)          # hybrid
    print(hits[0].source, hits[0].score, hits[0].cosine)
```

CI builds the wheel on Linux and macOS (Python 3.9 and 3.13), installs it into
a fresh virtualenv and runs `python/tests/` without and with NumPy.

## MCP / agent integration

cRAG is built to be driven by an LLM agent, and the wrapper already exists:
[`crag-mcp`](https://github.com/Simon-oid/cMCP/tree/main/tools/crag-mcp), a
reference MCP server in the sibling [cMCP](https://github.com/Simon-oid/cMCP)
project (a from-scratch C11 implementation of the
[Model Context Protocol](https://modelcontextprotocol.io/)). It links cRAG
statically and exposes the index as two MCP tools:

| Tool | Purpose |
|---|---|
| `crag_search` | Hybrid retrieval (cosine + BM25, RRF-fused) over the index |
| `crag_stats`  | Diagnostics: DB path, chunk count, file count, dimension |

```bash
make                                          # build cRAG objects first
git clone https://github.com/Simon-oid/cMCP ../cMCP
make -C ../cMCP crag-mcp CRAG_DIR=$(pwd)      # builds tools/crag-mcp/crag-mcp
```

Point any MCP host (e.g. Claude Code) at the resulting stdio binary and the
agent can search your corpus directly. The agent-side behaviour — the
abstention contract, cross-document reasoning, prompt-injection handling — is
validated end-to-end with Claude as the reasoning host in
[`docs/agent-validation.md`](docs/agent-validation.md) (15/15 graded answers).

## Project layout

```text
src/          chunker, store, search, embed/gen backends, indexer, query
include/      public headers
tests/        unit + integration tests; tests/manual/ holds graded golden sets
tests/eval/   BEIR harness + Python reference implementations
third_party/  vendored sqlite3.{c,h} (amalgamation, FTS5 enabled)
docs/         benchmarks, agentic-readiness, agent validation, performance report
.github/      CI workflow + issue/PR templates
```

## Documentation

| Doc | What's in it |
|---|---|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Module-by-module design, data flow, key constraints, full CLI reference. |
| [`ROADMAP.md`](ROADMAP.md) | What's shipped, what's deferred, and the measure-before-keep philosophy. |
| [`CHANGELOG.md`](CHANGELOG.md) | Release history. |
| [`docs/`](docs/) | Benchmarks, agentic-readiness plan, agent-in-the-loop validation. |

## Status &amp; roadmap

cRAG is **feature-complete for v0.1.0** and gated by `make check` (tests +
valgrind + ASan/UBSan + TSan + offline eval). Retrieval is at the architecture
ceiling for the personal/single-tenant target; the live levers are
generation/reasoning and the agent-side abstention contract, not new retrieval
machinery. Deferred work (HNSW, PDF/HTML, Windows port, configurable FTS
tokenizer) is gated on a real need — see [`ROADMAP.md`](ROADMAP.md).

## License

[MIT](LICENSE). © Simon-oid.
