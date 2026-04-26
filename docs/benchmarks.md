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

## Run 3 — `/usr/share/doc` after parallel + hybrid + md coalesce

Same corpus, re-run after the four big landings since Run 2:
markdown chunker coalescing, incremental reindex, hybrid retrieval,
and the parallel embed pool. 10-query golden set widened to fully
fair targets (kitty, lazygit, python-networkx, bison added) so
recall numbers are honest, not gated by missing packages.

### Indexing

| Metric | Run 2 (cosine, 1 worker) | Run 3 (parallel, 4 workers) |
|---|---|---|
| Files scanned | 448 | 448 |
| Chunks stored | 57 837 | **56 997** |
| Chunks dropped | 16 (context) | 16 (context) |
| Files partial | 2 | 2 |
| Cold wall time | 18 m 48 s | **11 m 27 s** (1.64×) |
| **Incremental rerun** (no changes) | n/a (pre-feature) | **36 ms** |
| DB size on disk | 275 MB | 278 MB (FTS5 +3 MB) |

The chunk count dipped slightly: the markdown coalesce reduces small
multi-header READMEs from N chunks to 1, but `/usr/share/doc` is
dominated by long files (kitty docs, networkx examples, lazygit
guides) where coalesce has no effect. Net: -840 chunks.

The 1.64× speedup at 4 workers matches the smaller `python-networkx`
benchmark: Ollama's GPU serializes hard past ~4 concurrent calls,
so adding more workers doesn't help.

The 36 ms incremental rerun is a `stat()` per file followed by a
`SELECT mtime,size` against the `files` table — no embedding, no
chunking, no inserts. With this in place, rebuilds become free for
unchanged corpora; only edited files get re-embedded.

### Query latency (k=5, warm, 10 queries)

| | cosine | bm25 | hybrid |
|---|---|---|---|
| min | 130 | 18 | 132 |
| p50 | 131 | 19 | 133 |
| p95 | 137 | 32 | 148 |
| max | 137 | 32 | 148 |

(All in milliseconds.) Cosine and hybrid are dominated by the
embedding HTTP round-trip to Ollama (~125 ms); BM25 skips the
embedder entirely, hence the 7× gap. Hybrid pays one more SQLite
pass over FTS5 on top of cosine — adds ~2 ms p50, ~11 ms p95.

### Retrieval spot-check (10 queries, fully fair)

10 queries, one per package, each package present with `.md`/`.txt`
docs in the index. Same query set across modes.

| Mode | recall@1 | recall@3 |
|---|---|---|
| cosine | 8/10 | 10/10 |
| **hybrid** | **9/10** | 9/10 |
| bm25 alone | n/a | n/a (not measured) |

Hybrid wins at @1 (lifts `systemd` from rank 2 → 1) but loses one
@3 hit. The single miss in both modes is the same package: `gnupg`.

### The gnupg "miss" is upstream of retrieval

Run 2 attributed the gnupg near-miss to "embedder limitation."
That was wrong. The actual cause:

```
$ ls /usr/share/doc/gnupg
DCO  DETAILS  examples  FAQ  HACKING  KEYSERVER  OpenPGP  README  TRANSLATE
```

Every file you'd actually want — README, FAQ, HACKING, KEYSERVER —
is **extension-less**. The `--ext md,txt` filter passes only
`examples/qualified.txt` and `examples/trustlist.txt` through, both
of which are sample config files, not docs about key signing. So
the index simply contains no gnupg material relevant to the query.

Hybrid amplifies this: BM25 surfaces packages whose READMEs *mention*
GnuPG (libseccomp, c-ares, sequoia-sqv all document GPG-signed
releases), pushing real-gnupg further down. RRF can't recover from
both pools agreeing.

Two takeaways for the roadmap:

- The `--ext` whitelist is too narrow for real-world doc trees.
  Common doc filenames (`README`, `FAQ`, `INSTALL`, `NEWS`,
  `HACKING`) are extension-less. A future flag could opt-in to
  these by name pattern. Tier 2 candidate.
- The current spot-check stops being meaningful for packages whose
  docs aren't in the index. Filtered to the 9 fully-indexed
  packages, hybrid is **9/9 @1**.

## Run 4 — `/usr/share/doc` with `--include-known-docs`

Same machine and corpus as Run 3, re-indexed with the new
`--include-known-docs` flag so extension-less doc files (README,
FAQ, HACKING, etc.) pass the walk filter. Same 10-query golden set.

### Indexing

| Metric | Run 3 | Run 4 (`--include-known-docs`) |
|---|---|---|
| Files scanned | 448 | **627** (+179 extension-less) |
| Files indexed | 448 | 626 (1 sweep-removed inert) |
| Chunks stored | 56 997 | **71 587** (+14 590) |
| Chunks dropped | 16 (context) | 16 (context) |
| Files partial | 2 | 2 |
| DB size on disk | 278 MB | ~340 MB est. |

The +179 files are the doc tail of /usr/share/doc that previously
vanished into the `--ext md,txt` filter: `gnupg/README` and friends,
plus a long list of `INSTALL`, `NEWS`, `AUTHORS`, `ChangeLog`
files. The whitelist is conservative — package-specific
extension-less names (gnupg's `KEYSERVER`, `DETAILS`, `OpenPGP`,
`TRANSLATE`) stay out.

### Retrieval spot-check (10 queries, fully fair)

| Mode | recall@1 | recall@3 |
|---|---|---|
| cosine | 9/10 | 10/10 |
| **hybrid** | **10/10** | **10/10** |

`gnupg` lifts to rank 1 under hybrid (was missing entirely in Run 3
because no gnupg docs were in the index). All ten target packages
hit at rank 1 under hybrid; the only sub-1 result is `gnupg` at
rank 2 under pure cosine, which hybrid cleans up. Clean sweep.

## Run 5 — same corpus, sliding_split no-progress fix

Same machine, same corpus, same 10-query golden set as Run 4. The
chunker bug investigated for TODO 2.1 turned out to be fired on
**every** multi-chunk file, not just the two flagged pathological
ones: the final partial-window iteration of `sliding_split` left
`pos = sec_end - overlap`, then the next iteration with
`take = overlap` looped at the same `pos`, emitting the trailing
fragment until `MAX_CHUNKS_PER_FILE = 512` saturated. Fix in
`src/chunk.c::sliding_split`: break when `split_at >= sec_end`,
and force progress when overlap eats the forward step.

A second bug surfaced once the chunker stopped inflating: in the
parallel pool path, `file_dropped` counted every leaf of the
recursive split tree (8 leaves per fully-failing depth-3 chunk),
which misclassified one-bad-chunk files as "fully dropped". Fixed
by classifying via insert-count delta instead.

### Indexing

| Metric | Run 4 (with bug) | Run 5 (fixed) |
|---|---|---|
| Files scanned | 627 | 627 |
| Files indexed | 625 | **626** |
| Chunks stored | 71 587 | **13 541** (-58 046, -81%) |
| Chunks dropped | 24 | 24 |
| Files partial | 2 | 3 |
| Files fully dropped | 1 (mis-classified) | **0** |
| Cold wall time | ~13 m | **5 m 19 s** (~2.5×) |
| DB size on disk | ~340 MB | **82 MB** (3.4× smaller) |

The 58 046 chunks removed were all sliding-split tail duplicates —
zero new information per chunk. The retrieval recall is unchanged
(see below) because every duplicate was a strict subset of an
existing chunk.

### Query latency (k=5, warm, 10 queries)

| | cosine | bm25 | hybrid |
|---|---|---|---|
| min | 54 | 13 | 57 |
| p50 | 56 | 16 | 62 |
| p95 | 60 | 26 | 68 |
| max | 60 | 26 | 68 |

(All in milliseconds.) Cosine and hybrid roughly halved (131 → 56
p50, 133 → 62 p50) — the brute-force scan now walks 13.5k rows
instead of 71.5k. BM25 was already FTS5-bound and only nudged
down (19 → 16 p50). Hybrid stays a few ms above cosine for the
extra FTS5 pass.

### Retrieval spot-check

Hybrid: **10/10 @1, 10/10 @3** — unchanged. Cosine: 9/10 @1,
10/10 @3 — unchanged. Removing duplicates removed nothing
informative.

## How to reproduce

```bash
# Pick the embedder and backend
export CRAG_EMBED_BACKEND=ollama
export CRAG_EMBED_MODEL=mxbai-embed-large

# Cold index (parallel)
rm -f /tmp/crag_bench.db
./crag index /usr/share/doc --ext md,txt --db /tmp/crag_bench.db --workers 4

# Incremental rerun (should print "files unchanged: 448" in milliseconds)
./crag index /usr/share/doc --ext md,txt --db /tmp/crag_bench.db --workers 4

# Query in any mode
for mode in cosine bm25 hybrid; do
    ./crag query "async I/O event loop in libuv" \
        --db /tmp/crag_bench.db -k 5 --search "$mode"
done
```

## Provenance

Run 1 and Run 2 captured 2026-04-24 against commit `acbd9e2`.
Run 3 captured 2026-04-25 against commit `b636f96` (parallel pool
+ hybrid + md coalesce + incremental all live).
Run 4 captured 2026-04-26 with `--include-known-docs` added.
Run 5 captured 2026-04-26 after the `sliding_split` no-progress
fix and the pool partial-vs-fully-dropped accounting fix.
