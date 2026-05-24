# cRAG agentic-readiness plan

How we get cRAG to a quality bar where an LLM agent (butlerbot,
Claude Code, or anything consuming `crag-mcp`) can rely on its
retrieval results without a human in the loop. This is not a
feature roadmap — see [`TODO.md`](../TODO.md) for that. This is
the *quality* track: the work that turns "the test suite is
green" into "an agent can trust this corpus search to ground its
answers for hours of autonomous operation."

## What "agent-autonomous" demands

An agent that issues `crag_search(query)` and writes code based on
the returned chunks has no human reviewing the relevance ordering.
If chunk #1 is plausible-looking but tangential, the agent acts on
it. The bar is therefore stricter than "the test passes":

1. **Deterministic results.** Same query, same corpus, same
   results — every time. Non-determinism is invisible bugs.
2. **Calibrated relevance.** When the top-k is "weak," the score
   must reflect that, so the agent can decide to ask another
   question or admit it doesn't know.
3. **Crash-safe writes.** `crag index` must survive `kill -9`
   mid-run; the next invocation must either recover or fail
   loudly. No silent partial state.
4. **No silent loss.** Indexing must not skip files that the
   `--ext` filter accepts; querying must not drop hits that
   match. Counted exhaustively.
5. **Robustness against weird inputs.** UTF-8 edge cases, binary
   garbage in `.md`, malformed embedding-API responses, oversized
   chunks, FTS5 query-syntax characters in user queries.
6. **Long-running stability.** Re-indexing the same corpus
   100 times must not grow the database or leak memory.

## Current state (2026-05-24, v0.1.0)

What we already have working in our favour:

- **Five test binaries** (`test_chunker`, `test_search`, `test_store`,
  `test_util`, `test_retrieval`) with the in-process hashed embedder
  for hermetic end-to-end coverage.
- **valgrind clean** under `make valgrind`.
- **WAL mode** on SQLite — crash durability is the engine's
  responsibility and it is reasonably good at it.
- **Single-threaded SQLite** (`SQLITE_THREADSAFE=0`) — write race
  conditions are structurally impossible. The `--workers N` pool
  only parallelises `embed_one`.
- **Bulk `export` / `import`** as JSONL — backup + migration path
  exists.
- **Documented benchmarks** in [`docs/benchmarks.md`](benchmarks.md)
  on a real corpus (`/usr/share/doc`, 448 files, ~58k chunks).
- **Reference MCP wrapper** (`crag-mcp` in cMCP) so the surface an
  agent actually sees is exercised.

What we don't have:

- **No automated quality regression.** Benchmarks are manual, run
  ad-hoc, not compared against a baseline on every change. A
  silent recall regression from a chunker tweak would not be
  caught.
- **No sanitizers.** Only valgrind, which is dynamic and slow.
- **No fuzzing** of the JSON parser (`util.c`), the FTS5 query
  escaping in `store.c`, or the embedding-response parsers.
- **No crash-safety tests.** Nobody has SIGKILL'd `crag index`
  mid-batch and verified recovery.
- **No soak.** No long-running test verifies that 100 re-index
  cycles don't grow disk or RSS unboundedly.
- **No agent-in-the-loop validation.** `crag-mcp` is registrable
  but nobody has watched Claude actually answer questions over a
  cRAG-indexed corpus and recorded which queries failed.
- **No baseline comparison.** We don't know how cRAG's hybrid
  retrieval compares to a reference BM25 (rank_bm25) or a
  reference dense vector (FAISS flat) on a public benchmark.

## The five quality axes

Each axis has a name, the gap from current state, concrete actions,
and an acceptance criterion that defines "done."

### Axis 1 — Retrieval quality calibration

**Current state.** Two manual measurements in `docs/benchmarks.md`:
small synthetic corpus (110 docs, recall@1=20/20) and
`/usr/share/doc` (448 files, hybrid spot-check 10/10 on
hand-checked queries). Numbers are reproducible by humans but not
asserted by the build.

**Gap.** A chunker change, an embedding-backend swap, an FTS5
schema tweak can silently regress recall. The benchmarks doc is
historical, not load-bearing. No automated comparison against any
external reference system — we don't know in absolute terms
whether cRAG is "BM25-baseline good" or "well below the floor."

**Actions.**

1. **Eval fixture corpus** *(2 days)*. A small, checked-in dataset
   under `tests/fixtures/eval/`:
   - 50–100 small text documents on 5–10 disjoint topics
   - 20 queries with hand-judged "correct" doc-id lists (top-3 per
     query, written down once and frozen)
   - Stored as JSONL: `{query, correct_doc_ids: [...]}`
   Choose synthetic-but-realistic over real-world: licensing-clean,
   easy to reason about, fast to iterate.
2. **`make eval` target** *(1 day)*. Indexes the fixture corpus
   with each of the three retrieval modes (cosine, bm25, hybrid),
   runs all queries, computes recall@1, recall@3, MRR per mode.
   Writes results to `tests/eval/results.csv` and compares against
   a checked-in baseline `tests/eval/baseline.csv`. Fail the build
   if any metric drops more than 5% vs baseline. Update baseline
   intentionally (commit message must say so) when a real
   improvement lands.
3. **Determinism harness** *(half day)*. Run the same query 100
   times back-to-back; assert top-k id list is identical every
   time. Same fixture corpus, all three modes.
4. **BEIR-subset calibration** *(2 days, quarterly cadence)*. Once
   a quarter, run cRAG against a public benchmark subset
   (suggested: `trec-covid` truncated to 1k docs, or
   `scifact`). Compare recall@10 against rank_bm25 and against
   `sentence-transformers/all-MiniLM-L6-v2` + Faiss. Goal isn't to
   beat them — it's to know how far behind we are, in absolute
   numbers, so the agent's confidence in cRAG results is
   calibrated.
5. **Score-distribution sanity** *(1 day)*. Add `crag query
   --debug` output that includes the score gap between top-k
   results. Agents can use the gap as a "weak answer" signal.

**Acceptance criterion.** `make eval` runs in CI on every push.
Quarterly BEIR-subset run documented in `docs/benchmarks.md`.
Recall regressions block merges.

### Axis 2 — Memory + concurrency safety beyond valgrind

**Current state.** valgrind clean across the five test binaries.
No sanitizer builds. SQLite vendored at `-O2 -w`, deliberately
unchecked.

**Gap.** Valgrind catches a useful subset (definite leaks, invalid
reads/writes) but misses:

- *UB silenziosa* in cRAG code — signed overflow in scoring math,
  alignment violations on float-array casts, OOB on stack chunk
  buffers.
- *Race conditions* in the embed pool. SQLite is single-threaded
  but the pthread pool around `embed_one` shares the work-item
  queue, libcurl easy handles per worker, and the recursive
  `CRAG_ECONTEXT` split fan-out feeds work back into the same
  pool. Valgrind cannot see ordering hazards.
- *Use-of-uninitialised-memory* on embedding-error paths or on
  malformed JSON branches.

**Actions.**

1. **`make test-asan`** *(2 hours)*. Rebuild with
   `-fsanitize=address,undefined`. Vendor sqlite3 with the same
   flags or suppress its findings via `tests/asan.supp`. Run the
   five binaries; fix everything that fires.
2. **`make test-tsan`** *(2 hours setup, however long the findings
   take)*. `-fsanitize=thread`. Stress the embed pool with
   `tests/test_retrieval` configured for 8 workers on a synthetic
   100-file corpus. Document findings; fix or annotate.
3. **GCC `-fanalyzer` weekly run** *(half day setup)*. Slow static
   analyser; weekly cron, not per-push.

**Acceptance criterion.** `make test-asan` and `make test-tsan`
both pass with zero unsuppressed findings. CI runs them on every
push.

### Axis 3 — Robustness under hostile / malformed input + crash safety

**Current state.** Tests cover happy paths. SQLite WAL provides
some crash safety. The hand-rolled JSON parser in `util.c` has no
adversarial test.

**Gap.** Several attack surfaces have never been exercised:

- **JSON parser** (`util.c`, 62 lines). Hand-parses embedding /
  generation API responses. A malformed response could crash the
  indexer mid-corpus.
- **FTS5 query escaping** (`store.c`). User queries reach FTS5
  literally; characters like `*`, `"`, `(`, `:` have meaning and
  must be escaped. Unescaped FTS5 syntax in a user query has
  unknown behaviour — at best a silent no-match, at worst a
  parser error that propagates as a crash.
- **Chunker UTF-8 edge cases**. BOM at start of file, lone
  surrogates, NUL bytes, mixed line endings, files that claim to
  be markdown but aren't, files with `### ` lines that span past
  `max_chars` boundaries.
- **`crag index` crash recovery**. WAL handles transaction
  durability but if you SIGKILL between transactions, are the
  `files` table and the `chunks` table consistent? Does the
  *next* `crag index` correctly resume / detect / replay?
- **Embedding-API failure injection**. What does cRAG do when the
  embedding API returns 500, returns a JSON object with a missing
  `embedding` field, returns a vector of wrong dimension, returns
  a vector with NaNs?

**Actions.**

1. **libFuzzer harnesses** *(2 days)*. Create `fuzz/`:
   - `fuzz_util_json.c` → `util_json_parse_*` on
     embedding-response shapes
   - `fuzz_chunker.c` → `chunk_file` on arbitrary byte sequences
   - `fuzz_fts_query.c` → the query escaping path before FTS5
   Build with `-fsanitize=address,undefined,fuzzer`; seed corpus
   from existing test fixtures. Initial 24h run per target.
2. **Crash-safety test** *(2 days)*. `tests/test_crash_safe.c`:
   - Build a 100-file corpus
   - Fork. Child runs `crag index`. After random delay (50–500ms),
     parent sends SIGKILL.
   - Re-run `crag index` (no `--force`). Assert: no DB corruption
     errors, no SIGABRT, all 100 files end up indexed when the
     loop finishes successfully. The files table and chunks table
     remain consistent (no orphan chunks referencing dead file
     ids, no files marked indexed_at without corresponding
     chunks).
   Repeat 20 times to surface flaky races.
3. **Embedding-API fault injection** *(1 day)*.
   `tests/test_hostile_embed.c`: stub the embed backend to return
   - HTTP 500 → indexer must skip the chunk, not corrupt the DB
   - JSON `{"error": "..."}` → same
   - JSON `{"embedding": []}` (empty vector) → reject with
     `CRAG_ESHAPE`
   - JSON `{"embedding": [NaN, NaN, ...]}` → reject; don't store
   - vector of wrong dimension vs existing chunks → reject with
     specific error
4. **Hostile-query test** *(1 day)*. `tests/test_hostile_query.c`:
   feed `crag_search` with queries containing FTS5 syntax
   (`"`, `*`, `(`, `:`, `+`, `-`, `^`, `NEAR`), Unicode (CJK,
   emoji, RTL), empty string, 100KB string, NUL bytes. Every
   case: either a normal result or a documented error code. No
   crash, no SQL injection equivalent.
5. **Resource exhaustion limits** *(half day)*. Document and
   enforce: max file size cRAG will chunk, max chunks per file,
   max embedding-vector dimension, max query length. Tests for
   each.

**Acceptance criterion.** Three fuzz harnesses run 24h with zero
crashes / hangs / leaks. Crash-safety test passes 20/20 runs.
Hostile-embed and hostile-query suites cover their listed cases.

### Axis 4 — Long-running stability

**Current state.** Tests run for seconds. Nobody has measured
what hours of re-indexing or thousands of queries does.

**Gap.** Likely fault classes:

- **Database file bloat** without `VACUUM`. WAL grows; chunks
  table accumulates if `store_sweep_missing` ever misses. After
  100 re-index cycles, what's the disk footprint?
- **Embed-pool worker leak** — pthreads created per index run,
  joined per index run? Spawn-only?
- **libcurl easy-handle leak** — each embed worker holds one; the
  pool shutdown path is the test.
- **FTS5 index degradation** — re-inserts without `optimize` can
  bloat the auxiliary tables; query latency drifts upward over
  time.
- **Query-side leak** — a per-query allocation that doesn't free
  on the no-results path.

**Actions.**

1. **`tests/soak/reindex_cycle.sh`** *(2 days)*. Driver script:
   - Build a 1000-file fixture corpus (mix of sizes)
   - Loop: `crag index`, run 100 queries, `crag reindex --force`,
     run 100 queries. Repeat for N hours (default 4h).
   - Every cycle, sample disk usage of the DB file, RSS of the
     indexer process, p50/p99 query latency.
   - Pass: DB size growth bounded (≤ 10% over the run after
     cycle 5), RSS stable, latency within 2x of cycle-1 baseline.
2. **Query-only soak** *(1 day)*. Variant: index once, run 100k
   queries in a loop, sample RSS every 1000 queries. Catches the
   query-side leak.
3. **Embed-API failure soak** *(1 day)*. Variant: index a corpus
   where 10% of embed calls fail randomly (use a flaky stub
   backend). Verify the indexer survives, completes the run,
   leaves a consistent DB.
4. **Run nightly** *(folded into CI from Axis 1)*. The 4h reindex
   soak is incompatible with per-push CI. Nightly cron, results
   in `soak-history.csv`.

**Acceptance criterion.** 4h reindex-cycle soak passes the criteria
above. Query-only soak shows zero RSS growth over 100k queries.

### Axis 5 — Real agent in the loop

**Current state.** `crag-mcp` exists and works (8/8 conformance
checks against TS reference client). Nobody has watched Claude
actually drive it on a useful corpus.

**Gap.** The specific failure modes only an agent can find:

- Tool descriptions in `crag-mcp` (`crag_search`, `crag_stats`)
  might be too terse → the model picks the wrong tool or formats
  arguments badly.
- The result format (chunks with scores + snippets) might be
  hard for the model to reason about — e.g., the model picks the
  first chunk by position, not by score.
- When all results are weak, the model might confidently use them
  anyway because the API doesn't surface a "weak" signal.
- Error messages from `crag-mcp` (when the DB is missing, when
  the query is empty) might not be actionable.

**Actions.**

1. **Register `crag-mcp` with Claude Code** *(30 minutes)*.
   Indexed corpus: pick something the agent will actually need to
   know — your codebase, your notes, the cMCP + cRAG docs. Add
   to `~/.claude/mcp_servers.json` (or equivalent). Document the
   exact config in `docs/agent-validation.md`.
2. **Question playbook** *(1 day to design, ongoing to run)*.
   ~15 questions whose answers are in the indexed corpus and
   whose ground truth you know:
   - 5 factual ("what protocol version does cMCP pin?")
   - 5 navigational ("which file implements the FTS5 escaping?")
   - 5 abstractive ("summarise how cRAG handles chunker
     coalescing")
   Run the full playbook in a fresh Claude session, weekly.
   Record: did Claude answer correctly? Did it cite the right
   chunks? Did it issue follow-up `crag_search` calls when the
   first was weak?
3. **Tool-description iteration** *(continuous)*. Every failed
   playbook question that traces to a tool-description issue or
   a result-format issue: open a `crag-mcp/` issue, fix in the
   next sprint, regress on next run.
4. **Compare against a baseline** *(half day)*. Run the same
   playbook with Claude using *only* its built-in tools (no
   cRAG). The delta is the value cRAG adds. If it isn't large
   and growing, the retrieval is not pulling its weight.
5. **butlerbot integration test** *(deferred until butlerbot
   exists)*. Port the playbook to butlerbot's autonomous loop;
   that becomes the agent-readiness regression suite.

**Acceptance criterion.** Weekly playbook run achieves ≥ 13/15
correct answers, with citation. Failures are classified as
"corpus didn't contain answer," "retrieval missed the chunk,"
or "model misused tool" — each category tracked separately so
we know whether to improve cRAG, the corpus, or the descriptions.

## Sequencing

Priority order if I were picking. Effort estimates include the
test-and-fix cycle.

| # | Axis | Effort | Why this order |
|---|------|--------|----------------|
| 1 | **Eval fixture + `make eval`** (Axis 1.1 + 1.2) | 3 days | Single biggest quality blind spot. Without an automated relevance baseline, every retrieval-touching change is flying blind. |
| 2 | **Sanitizers** (Axis 2.1 + 2.2) | 2 days | Cheap to add, permanent benefit on every test run. Most likely to find C-level bugs we don't know about. |
| 3 | **Agent-in-the-loop** (Axis 5.1 + 5.2) | 1.5 days + continuous | Free signal you can start today. Discovers the failure classes no offline test can: tool-description and result-format issues. |
| 4 | **Crash safety** (Axis 3.2) | 2 days | If `crag index` corrupts the DB on a SIGKILL, the agent can't recover. Catches a real-world failure mode (laptop sleep, OOM kill). |
| 5 | **CI gate** (Axis 1.2 + 2 + 3 wired together) | 1 day | Consolidates everything above into automatic enforcement. Do it once the previous items have something to enforce. |
| 6 | **Fuzzing** (Axis 3.1) | 2 days + 3× 24h runs | Likely to find bugs in `util.c` JSON parser. Lower priority than the eval baseline only because retrieval-quality regression hurts more than a parser crash on hostile input. |
| 7 | **Hostile-embed + hostile-query** (Axis 3.3 + 3.4) | 2 days | Filling in the gaps fuzzing won't reach. |
| 8 | **Soak** (Axis 4) | 4 days + nightly runtime | Slowest to give feedback. Important for long-running agent use; defer until faster cycles are clean. |
| 9 | **BEIR-subset calibration** (Axis 1.4) | 2 days, quarterly | Absolute calibration. Run once it's clear cRAG's *relative* recall is stable, so the absolute number means something. |

Total: ~20 person-days of focused work plus continuous overhead.

## Explicitly out of scope

- **State-of-the-art retrieval research.** cRAG is "hybrid BM25 +
  cosine RRF." If a future agent needs ColBERT-style late
  interaction or learned sparse retrievers, that's a different
  project. Stay within the architecture.
- **Distributed indexing.** One process, one DB. Multi-machine
  fan-out is not on the agent-readiness critical path.
- **Replacing SQLite or FTS5.** Both are mature, audited, well
  understood. Spend the time on cRAG-specific code, not on
  re-fighting battles SQLite already won.
- **Replacing the chunker.** The markdown coalescing fix in
  Phase 1.2 (cRAG TODO) is the current production-grade choice.
  Don't churn unless eval recall says otherwise.
- **Pure-C HNSW.** Tier 3 "deferred" item — not blocking agent
  use; at our corpus sizes the linear cosine scan is fast enough.
- **Performance optimisation past `make eval` baselines.**
  Latency is not the agent-readiness bottleneck — correctness is.

## Tracking

Each axis above gets one entry in [`TODO.md`](../TODO.md) under a
new "Tier 4 — agentic readiness" section. As tasks land, the
acceptance criterion above is the bar for marking them done.
