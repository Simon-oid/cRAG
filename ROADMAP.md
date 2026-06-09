# Roadmap

cRAG **v0.1.0 is feature-complete** for its target — a personal / single-tenant
RAG engine driven by an LLM agent. This document records what shipped, what is
deliberately deferred, and the principle that governs both.

## Guiding principle: measure before you keep

Features are added only when there is a **real need** or a **measured number**
that justifies them. Every deferred item below is gated on an explicit
trigger ("build it when X happens"), not a vague "someday." This keeps the
dependency surface small and the exactness/determinism guarantees intact.

Two recurring conclusions from the project's own measurements shape the
roadmap:

1. **Retrieval is at the architecture ceiling.** On BEIR SciFact, exact cosine
   scores nDCG@10 **0.7407** with `mxbai-embed-large`, verified bit-for-bit
   against NumPy and FAISS `IndexFlatIP` — the engine adds zero retrieval loss
   over its embedder. Chasing a higher number means changing the *embedder*, not
   the engine.
2. **Cross-encoder reranking does not help this setup** — measured across both a
   saturated regime (SciFact, 0.7407 → 0.7248) and a high-headroom one
   (NFCorpus, 0.3746 → 0.3284). The infrastructure stays available but off by
   default. The live levers are generation/reasoning and the agent-side
   abstention contract, not new retrieval machinery.

## Shipped (v0.1.0)

- **Markdown-aware chunking** with header/code-fence handling, section
  coalescing, and a sliding-window fallback.
- **Pluggable embedding & generation backends** (`local`, `ollama`, `openai`,
  `anthropic`) via vtables.
- **SQLite store** with an FTS5 shadow table and an incremental-reindex
  `files` table.
- **Three retrieval modes** — cosine, BM25, and hybrid (RRF, k=60, default).
- **Parallel indexing** — an in-process pthread pool around `embed_one`
  (`--workers N`), with `CRAG_ECONTEXT` recursive split fan-out.
- **Interrupt-safe indexing** — periodic commits bound the blast radius of a
  kill or crash; per-file drop diagnostics.
- **Embedder-aware confidence gates** for generation (`bge-m3` → 0.38,
  else 0.60).
- **Query-time snippet highlighting** (ANSI on a TTY, `<<term>>` when piped).
- **Bulk export/import** (`JSONL`, embedder-portable).
- **Robustness gates** — `make check` runs the suite under valgrind, ASan/UBSan,
  and TSan; a parser fuzzer (`test_fuzz`) that already caught and fixed a real
  heap overflow; a determinism test; prompt-injection structural tests.
- **Offline eval gate** — `make eval-ci`, a deterministic, server-free
  nDCG/Recall/MRR regression floor that runs in CI.
- **BEIR evaluation** — `make eval` against SciFact / NFCorpus with Python
  reference implementations for cross-checking.
- **Agent-in-the-loop validation** — end-to-end runs through an MCP wrapper with
  a real agent host (see [`docs/agent-validation.md`](docs/agent-validation.md)).

## Deferred — gated on a real need

These are genuinely useful at a *different* scale or for a *different* corpus
than the current target. Each is left out on purpose, with its trigger named.

### Configurable FTS tokenizer

Expose the FTS5 tokenizer via `CRAG_FTS_TOKENIZER=unicode61|trigram|porter`.

- **Why not now:** the default `unicode61` already handles English and Romance
  languages well, including Italian (the deployment target). `trigram` is for
  CJK, which is out of scope.
- **Trigger:** indexing a CJK corpus, or needing `porter` stemming for an
  English keyword-heavy corpus. **Effort:** trivial (a pragma string at FTS
  table creation; the cost is reindexing the existing DB).

### Variable input formats (PDF / HTML)

Ingest PDF/HTML in addition to `.txt`/`.md`.

- **Why not now:** the easy path (shell out to `pdftotext` / `lynx -dump`) adds
  a runtime dependency to a project whose value is "pure C11, minimal deps." The
  current design keeps PDF *outside* cRAG and documents the one-liner pre-ingest
  step (`find . -name "*.pdf" -exec pdftotext {} {}.txt \;`).
- **Trigger:** a real consumer must ingest PDF/HTML and external preprocessing
  is unacceptable. **Effort:** small if shelling out; weigh against portability.

### ANN index (HNSW) — a *latency* optimization

Replace the brute-force cosine scan with an approximate index (HNSW) for
sub-linear search.

- **Why not now:** this solves a scale problem that does not exist in the
  target envelope. Measured: brute-force does ~57k rows in ~124 ms, and at the
  target scale the scan is dominated by the embedding round-trip and the LLM
  call — it disappears in the noise. HNSW is approximate (recall drops below
  100%), so it would trade away the exactness/determinism guarantee to optimize
  the one part that isn't the bottleneck. It also does **not** reduce memory
  (the graph sits *on top* of the float32 vectors).
- **Trigger:** the corpus grows past ~1M chunks and scan latency becomes
  user-visible. At that point HNSW pairs with quantization (below).

### Vector quantization — a *memory* optimization

Store int8 (or PQ) vectors instead of float32 to cut RAM.

- **Why not now:** float32 1024-dim is ~4 KB/vector; even ~100k chunks is
  ~400 MB. The target corpus fits in RAM comfortably. Quantization rounding is
  platform/SIMD-sensitive and could silently break the byte-identical
  determinism the project tests for — it would have to be re-validated against a
  float32 oracle, not assumed free.
- **Trigger:** the corpus no longer fits in RAM (roughly ~1M+ chunks).

### Exact-scan SIMD / threading + a float32 oracle

The cheapest forward-looking lever: SIMD/multithread the *exact* cosine scan and
add a float32 oracle harness that acts as a recall + determinism regression gate
for any future backend. It keeps exactness, costs no virtue, and defers the
entire ANN question. **Trigger:** the exact scan becomes a measured bottleneck
before the corpus outgrows RAM.

### Windows port

Currently Linux/macOS (POSIX `nftw`, pthreads). **Trigger:** a Windows consumer.

## Out of scope by design

The enterprise/multi-tenant story — distributed sharding, GPU-accelerated ANN,
multi-billion-vector indexes — is explicitly **not** a goal. cRAG optimizes for
exactness, determinism, a tiny dependency surface, and a single portable C
binary. Those are mutually exclusive with the approximations that large-scale
serving requires. If that scale ever becomes the target, it is a different
project, not a flag on this one.
