# cRAG Roadmap

cRAG v0.1.0 is feature-complete: Tier 1 (the items that materially
shape how the project looks and feels) is fully closed. Tier 2 is
optional polish; Tier 3 is large-scope deferred work. Phase 2 (the
MCP wrapper) can start whenever the user gives the word.

## Tier 1 — blocking "cRAG is done" — ALL DONE

Each subsection below kept for traceability — what shipped, where,
and what was measured. All six items closed.

### 1.1 Parallel indexing — DONE
- Shipped: `--workers N` flag in `crag index`. Persistent pthread pool
  spun up once at index start, drained per file. Workers call
  `embed_one`; the writer (main thread) does `store_insert` and the
  CRAG_ECONTEXT split-fan-out, with split halves funneled back into the
  pool round-by-round so they get parallelised too.
- Pool is bypassed when the backend exposes `embed_many` (openai
  batches at the HTTP layer already) or when `workers <= 1`.
- Measured: `/usr/share/doc/python-networkx` (1 huge token-dense file
  + 12 tiny READMEs, ~2060 final chunks after recursive splits)
  76.3s → 50.5s with 4 workers (1.51×); 8 workers ~49s — Ollama
  serializes hard at the GPU level past ~4 concurrent calls. Chunk
  counts identical between worker counts.
- Still owed: re-bench `/usr/share/doc` cold and update
  `docs/benchmarks.md` (rolled into 1.5).

### 1.2 Markdown chunker coalescing — DONE
- Shipped in `da6700f`. `src/chunk.c` markdown branch now walks
  header boundaries with a `sec_start` / `safe_end` pair, accumulates
  consecutive sections until adding the next would overflow
  `max_chars`, and falls back to `sliding_split` for sections that
  individually exceed the limit. Code-fence detection unchanged.
- The old `test_markdown_splits_on_headers` (which pinned the
  per-header behavior) was replaced with two corpus-sized tests:
  short md → 1 chunk, oversized → splits at header boundary.
- Verified on `tests/manual/corpus`: 5 md files of ~2.5 KB each go
  from 6 chunks/file → 2 chunks/file; recall@3 stays at 100%, MRR
  0.975. Full test suite 227/227.

### 1.3 Real implementation of `local` embedding backend — DONE
- Already shipped in `a4c9052`. `src/embed_local.c` POSTs
  `{"content":"..."}` to llama.cpp's `/embedding` endpoint and pulls
  the `embedding` floats out of the response with
  `json_extract_floats`. Default URL is
  `http://localhost:8080/embedding`; override via `CRAG_EMBED_URL`.
- Verified: parser handles all three llama.cpp response shapes
  (legacy `{"embedding":[…]}`, modern `[{"embedding":[…]}]`, and
  OpenAI-compat `{"data":[{"embedding":[…]}]}`). End-to-end indexed
  `tests/manual/corpus` (10 chunks) against a mock server returning
  the modern array shape; query returned ranked results.
- Docs were stale, calling it a stub. CLAUDE.md fixed.

### 1.4 README.md — DONE
- One-page landing covering: what cRAG is, build/test, five-minute
  Ollama quickstart (index + query + generate), backend matrix
  (local/ollama/openai/anthropic), three search modes, indexing
  guarantees (markdown-aware, incremental, interrupt-safe, parallel),
  Run 3 numbers headline table, layout, status, MIT.
- Linked to `docs/benchmarks.md`, `CLAUDE.md`, `TODO.md`, `LICENSE`.
  Did **not** link to `docs/research/study.md` — that path is in
  `.gitignore` and not visible to GitHub readers.

### 1.5 Re-benchmark `/usr/share/doc` with hybrid + incremental — DONE
- Re-ran the cold rebuild against `b636f96` with `--workers 4` and md
  coalesce live: **18m48s → 11m27s** (1.64×), 57 837 → 56 997 chunks
  (-840 from md coalesce on small READMEs), same 16 context-drops,
  same 2 partial files.
- Incremental rerun on the unchanged corpus: **36 ms** — `stat()` +
  `SELECT mtime,size` per file, no embedding.
- Query latency p50: cosine 131 ms / hybrid 133 ms / **bm25 19 ms**
  (BM25 skips the embedder).
- 10-query spot-check (kitty, lazygit, python-networkx, bison added
  for fully-fair targets): cosine 8/10@1, hybrid 9/10@1.
- The "gnupg → sequoia-sqv" miss is upstream of retrieval: gnupg's
  README/FAQ/HACKING are extension-less, so `--ext md,txt` excludes
  them. Filtered to the 9 fully-indexed packages, hybrid is 9/9@1.
  Added as a Tier 2 candidate (extension-less doc filenames).
- `docs/benchmarks.md` now has a Run 3 section with all the above.

### 1.6 Update `docs/research/study.md` §8 — DONE
- §8 split into 8.1 (Shipped since the original draft), 8.2 (Still
  open), 8.3 (Phase 2 — MCP wrapper).
- 8.1 covers incremental reindex, hybrid retrieval, parallel
  indexing, and md coalesce — each with the design choice that
  actually shipped (RRF k=60 not α-weighted, files table with
  mtime+size, pthread pool around `embed_one` with split fan-out,
  header-section coalesce mirroring the plain-text pass).
- 8.2 lists the remaining open items: ANN/HNSW, PDF/HTML, Windows,
  the `--ext` whitelist gap, the two pathological files, FTS
  tokenizer, snippet highlighting.
- TOC updated to surface the new subsections.

## Tier 2 — polish

Good-to-haves. Skip any that feel like busywork for your goals.

### 2.1 Investigate the two pathological files
- `libsamplerate/index.md` and `lazygit/Config.md` each still lose
  one chunk even after depth-3 recursive split. Figure out what's
  peculiar (long URLs? unbreakable whitespace? tables?). Either
  raise depth cap or add a byte-level fallback split.
- Low user impact (2 files out of 448 = 0.4%) but worth documenting
  what edge case triggers it.

### 2.2 Index extension-less doc files
- **Why:** Run 3 turned up that gnupg's actual docs (`README`, `FAQ`,
  `HACKING`, `KEYSERVER`) have no extension and slip past
  `--ext md,txt`. Same is true for many distro packages
  (`README`, `INSTALL`, `NEWS`, `LICENSE`, `ChangeLog`). The
  retrieval spot-check can't measure these.
- **Shape:** Add an opt-in flag (e.g. `--include-known-docs`) that
  also matches a small whitelist of well-known basenames regardless
  of extension. Or accept `--ext "md,txt,"` (trailing comma →
  no-extension files), or simply teach the matcher to treat known
  basenames as text.
- **Success criterion:** With the new flag on, `gnupg/README` and
  `gnupg/FAQ` end up in the index; the gnupg query returns gnupg
  at rank 1 under hybrid.

### 2.3 Query-time snippet highlighting
- Right now the text output dumps the full chunk. For BM25 and
  hybrid, FTS5 has a `highlight()` function that wraps matched terms.
  Purely cosmetic but makes the retrieval visibly explainable.
- Only for text format; JSON output stays clean.

### 2.4 `crag reindex` subcommand
- Right now `index` handles both fresh and incremental cases. A
  dedicated `reindex` that forces everything (alias for `index
  --force`) is easier to document and to use scripting against.
- Or drop the idea — `--force` is already fine. The README went
  with `--force` only; revisit if scripting against it gets clumsy.

### 2.5 Bulk export / import for the store
- `crag export --format jsonl` to dump chunks (without vectors) and
  `crag import` to reload. Useful for debugging and for sharing an
  index without re-running expensive embedders. Low priority but a
  nice demo feature.

## Tier 3 — deferred

Large scope. Only tackle if you specifically want the challenge.
None of these block MCP.

### 3.1 ANN index (HNSW)
- Needed at >500k chunks. Current brute-force cosine does 57k rows in
  124 ms, which is fine for today. An HNSW serialized into a SQLite
  BLOB is a genuinely interesting exercise but 2-3 days of work with
  a test harness, and produces no user-visible improvement on the
  corpora you have.

### 3.2 PDF / HTML input
- Easiest path: shell out to `pdftotext` / `lynx -dump` and feed the
  result through the existing chunker. Adds a runtime dependency —
  weigh against the portability goal.
- Alternative: keep PDF out of cRAG, document the one-liner users
  can prepend (`find . -name "*.pdf" -exec pdftotext {} {}.txt \;`).

### 3.3 Windows support
- `nftw`, `strcasecmp`, `_GNU_SOURCE` all need replacements. Doable
  with a small `platform.h` abstraction. Low priority unless
  someone actually asks.

### 3.4 Configurable FTS tokenizer
- Default `unicode61` handles English and Romance languages well. For
  CJK, `trigram` works better. Expose via env
  `CRAG_FTS_TOKENIZER=unicode61|trigram|porter`. Trivial to add when
  a user needs it.

## Done (for reference)

Chronological. `--ext` is `md,txt` throughout unless noted.

- Chunker plain-text coalescing (`e59ee47`).
- Interrupt-safe commits, CRAG_ECONTEXT recovery, partial/drop
  reporting (`a8e63c4`).
- Git tree cleanup (`acbd9e2`).
- Benchmark snapshot on `/usr/share/doc` at cold scale (`0e9f423`).
- Incremental reindex: files table, skip unchanged, sweep missing,
  `--force` flag (`2a13e85`).
- Hybrid retrieval: FTS5 chunks_fts + BM25 + Reciprocal Rank Fusion,
  `--search cosine|bm25|hybrid` (`ba5f717`).
- Parallel indexing: pthread embed pool with CRAG_ECONTEXT splits
  fanned back through the pool, `--workers N` (default 1)
  (`e9e5639`).
- Markdown chunker coalescing: short md files no longer fragment
  per-header; large ones still split at header boundaries
  (`da6700f`).
- Local embedding backend already wired to llama.cpp `/embedding`
  in the MVP commit (`a4c9052`); verified end-to-end (`b636f96`
  fixed the stale "stub" docs).
- `/usr/share/doc` re-benchmark under parallel + hybrid + md
  coalesce + incremental; gnupg miss correctly attributed to
  `--ext` filter, not retrieval (`546f358`).
- README.md added; `docs/research/study.md §8` rewritten in place
  (file is `.gitignore`d, local-only) — both close out Tier 1.

## Phase 2 (out of scope until Tier 1 is clear)

- MCP server wrapper: thin stdio JSON-RPC exposing `crag_index`,
  `crag_query`, `crag_stats` as tools. Optional intelligent gating
  so an agent only calls the embedder when query confidence
  warrants it. Do not start until the user explicitly says cRAG
  itself is done.
