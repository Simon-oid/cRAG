# cRAG Roadmap

Remaining work to take cRAG from "working MVP" to "portfolio-ready
complete." Ordered by impact-per-effort. Once Tier 1 is clear, cRAG
is done and Phase 2 (MCP wrapper) can start.

## Tier 1 — blocking "cRAG is done"

These are the items that materially change how cRAG looks and feels as
a finished project. Ship these before MCP.

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

### 1.2 Markdown chunker coalescing
- **Why:** Mirror of the plaintext coalesce fix. The md chunker splits
  on every header even when the whole file fits in `max_chars`, so
  short READMEs get fragmented for no reason. Observed during the
  rag-test-data run before the .txt fix; md path was never touched.
- **Shape:** In `src/chunk.c`, walk header sections. Coalesce
  consecutive sections into one chunk until adding the next would
  overflow `max_chars`. Keep code-fence preservation intact.
- **Watchouts:** `test_markdown_splits_on_headers` pins the old
  behavior. Rewrite it to a corpus-sized test (small files coalesce,
  large ones still split on headers).
- **Success criterion:** New test green; existing retrieval quality
  on rag-test-data unchanged.

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

### 1.4 README.md
- **Why:** There's no README. For a portfolio piece, the GitHub
  landing page is the entire first impression.
- **Shape:** One page covering: what cRAG is, `make && ./crag`,
  five-minute quickstart (index + query + generate), backend matrix,
  link to `docs/research/study.md` for the deep dive. Keep it
  focused — CLAUDE.md and study.md already carry the heavy weight.
- **Watchouts:** Don't duplicate what's in CLAUDE.md. README is for
  readers deciding whether to read further; CLAUDE.md is for
  collaborators.

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

### 1.6 Update `docs/research/study.md` §8
- **Why:** §8 still lists incremental reindex and hybrid retrieval as
  "future work." They're done.
- **Shape:** Mark done items, add short notes on the chosen design
  (FTS5 external-content + RRF k=60; files table + mtime/size), and
  add new future-work entries for whatever Tier 2/3 items remain
  after Tier 1.
- **Quick win — do this last,** right before calling cRAG complete.

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

### 2.2 Query-time snippet highlighting
- Right now the text output dumps the full chunk. For BM25 and
  hybrid, FTS5 has a `highlight()` function that wraps matched terms.
  Purely cosmetic but makes the retrieval visibly explainable.
- Only for text format; JSON output stays clean.

### 2.3 `crag reindex` subcommand
- Right now `index` handles both fresh and incremental cases. A
  dedicated `reindex` that forces everything (alias for `index
  --force`) is easier to document and to use scripting against.
- Or drop the idea — `--force` is already fine. Decide based on how
  the README ends up reading.

### 2.4 Bulk export / import for the store
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

- Chunker plain-text coalescing (commit `e59ee47`).
- Interrupt-safe commits, CRAG_ECONTEXT recovery, partial/drop
  reporting (`a8e63c4`).
- Git tree cleanup (`acbd9e2`).
- Benchmark snapshot on `/usr/share/doc` at cold scale (`0e9f423`).
- Incremental reindex: files table, skip unchanged, sweep missing,
  `--force` flag (`2a13e85`).
- Hybrid retrieval: FTS5 chunks_fts + BM25 + Reciprocal Rank Fusion,
  `--search cosine|bm25|hybrid` (`ba5f717`).
- Parallel indexing: pthread embed pool with CRAG_ECONTEXT splits also
  fanned through the pool, `--workers N` (default 1).
- Markdown chunker coalescing: short md files no longer fragment
  per-header; large ones still split at header boundaries.
- Local embedding backend already wired to llama.cpp `/embedding`
  in the MVP commit; verified end-to-end.
- /usr/share/doc re-benchmark under parallel + hybrid + md coalesce
  + incremental; gnupg miss correctly attributed to `--ext` filter,
  not retrieval.

## Phase 2 (out of scope until Tier 1 is clear)

- MCP server wrapper: thin stdio JSON-RPC exposing `crag_index`,
  `crag_query`, `crag_stats` as tools. Optional intelligent gating
  so an agent only calls the embedder when query confidence
  warrants it. Do not start until the user explicitly says cRAG
  itself is done.
