# cRAG Benchmarks

Human-verifiable measurements on the author's machine. Numbers are
load-bearing only in the sense of "this is what we saw when the
harness was run"; reproduce locally before quoting.

## Machine

- CPU: AMD Ryzen 5800X
- GPU: AMD Radeon RX 7800 XT 16 GB
- Embedding server: Ollama (local)
- Embedding model: `mxbai-embed-large` (1024-dim, 512-token window)

## Run 1 — `~/rag-test-data` (small, synthetic)

110 short Italian "Smart City OS" docs across 5 categories
(architettura, trasporti, energia, sicurezza, ambiente).

| Metric | Before coalesce fix | After coalesce fix |
|---|---|---|
| Files indexed | 110 | 110 |
| Chunks stored | 330 (3 / file) | **110 (1 / file)** |
| Chunks dropped | 0 | 0 |
| Indexing wall time | 5.8 s | **2.9 s** |
| Fair recall@1 / @3 | 20/20 | 20/20 |

The 3x chunk inflation came from the plain-text chunker emitting one
chunk per paragraph (`\n\n` boundary) even when the whole file fit in
`max_chars`. Fixed in `e59ee47` — paragraphs now coalesce until they
would overflow `max_chars`.

## Run 2 — `/usr/share/doc` (realistic, on the host)

448 `.md` + `.txt` files across 204 installed packages — ~10.9 MB of
mixed-quality real documentation (READMEs, man-pages-as-text,
changelogs, tutorials).

### Indexing

| Metric | Value |
|---|---|
| Files scanned | 448 |
| Chunks stored | 57 837 |
| Chunks dropped | 16 (all context-length) |
| Files partial | 2 (`libsamplerate/index.md`, `lazygit/Config.md`) |
| Files fully dropped | 0 |
| Wall time | 18 m 48 s |
| Throughput | ~51 chunks/sec (~0.4 files/sec) |
| DB size on disk | 275 MB |

The bottleneck is Ollama's per-call embedding latency, not cRAG's
CPU work. `embed_many` is `NULL` for the Ollama backend, so every
chunk is a separate HTTP POST.

### Effect of the robustness fixes (commit `a8e63c4`)

A prior run on the same corpus, at the same configuration but before
the robustness fixes, was killed at the 10-minute mark and left the
database with **0 chunks** — a single wrapping transaction meant the
process-kill lost all committed-in-memory work. That run also
reported **1 364 silent drops** with no per-file diagnostics.

After the fixes:
- Periodic commit every 20 files bounds the interrupt blast radius.
- `CRAG_ECONTEXT` + recursive split recover chunks that tokenize
  past the 512-token model window.
- `partial:` warnings and the final summary surface which files are
  incomplete.

Net result on the same corpus: 31 383 → 57 837 chunks retained
(the difference is completion; the 10-minute prior run did not
reach all files), and drops fell from 1 364 silent to **16
reason-classified**.

### Query latency (k=5 against the 275 MB DB)

Warm, wall-clock, 10 representative queries. Query embedding goes
through Ollama; cosine scan is SQLite + `search_cosine` over all
~57k rows.

| | ms |
|---|---|
| Cold (first call) | 636 |
| Warm min | 123 |
| Warm p50 | 124 |
| Warm p95 | 127 |
| Warm max | 127 |

The cold-run overhead is mxbai-embed-large loading into VRAM.
Subsequent calls are dominated by the HTTP round-trip to Ollama
plus the brute-force cosine scan.

### Retrieval spot-check (fair queries only)

Of 10 queries, 4 were disqualified (no docs for that package exist
in `/usr/share/doc` on this machine). On the remaining 6:

- 5 clean top-1 hits (libuv, Linux-PAM, dav1d, cryptsetup, systemd).
- 1 near-miss: "GnuPG key signing and verification" retrieved
  `sequoia-sqv` at rank 1 instead of `gnupg` — both are GPG
  signature tools. Embedder limitation, not cRAG.

## How to reproduce

```bash
# Pick the embedder and backend
export CRAG_EMBED_BACKEND=ollama
export CRAG_EMBED_MODEL=mxbai-embed-large

# Index
rm -f /tmp/crag_bench.db
./crag index /usr/share/doc --ext md,txt --db /tmp/crag_bench.db

# Query (warm up then measure)
for q in "async I/O event loop in libuv" \
         "PAM module authentication stack" \
         "cryptsetup LUKS partition encryption"; do
    time ./crag query --db /tmp/crag_bench.db -k 5 "$q" > /dev/null
done
```

## Provenance

Benchmarks captured 2026-04-24 against commit `acbd9e2`.
