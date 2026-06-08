# Manual quality tests

Human-verifiable tests for cRAG. The unit tests in `tests/test_*.c` prove
the code compiles, the cosine math is right, and the store round-trips
bytes. They do **not** prove the retriever actually finds the right
document, or that the generator answers honestly. That's what this
directory is for.

You run them, you judge the output.

## Philosophy

- **Nothing here auto-passes.** A green unit test can mean "the code is
  correct" OR "the test is weak." These scripts are designed so that
  quality regressions are visible, not hidden.
- **Small corpus, distinct topics.** Five short docs on wildly different
  subjects (pasta, planets, kernels, Roman law, jazz). If retrieval
  confuses these, it's broken.
- **Golden set is short but real.** Twenty queries. They are phrased the
  way a user would ask them, not the way the docs are written — so the
  embedder has to actually bridge semantics, not just match keywords.
- **Three levels of depth.** Smoke → recall → generation. Spend seconds,
  minutes, or a careful read depending on how deep you want to go.

## The scripts

### `smoke.sh` — Level 1: is it wired up?

```bash
bash tests/manual/smoke.sh [ollama|openai|local]   # default: ollama
make manual-smoke                                   # same thing
```

Indexes the corpus into a throwaway DB, runs 10 sample queries (8
topical + 2 deliberately out-of-corpus distractors), prints the top-3
for each. Color-coded so obvious misses jump out. Takes ~10 seconds.

Use this as the first thing after any change that could break the
retrieval path.

### `recall.sh` — Level 2: how good is retrieval?

```bash
bash tests/manual/recall.sh [ollama|openai|local]
make manual-recall
```

Runs all 20 queries from `golden.tsv` and computes three numbers:

- **recall@1** — fraction of queries where the *right* document is
  ranked first.
- **recall@3** — fraction where the right document is in the top 3.
- **MRR@5** — mean reciprocal rank over top-5 (penalizes queries where
  the answer is technically found but buried).

The script exits non-zero if recall@3 drops below 80%. That threshold
is advisory; the real signal is the per-query breakdown and the
failure list at the bottom.

### `gen.sh` — Level 3: does the model answer honestly?

```bash
bash tests/manual/gen.sh [ollama|openai|local]
make manual-gen
```

For each of six handpicked questions (including one deliberately
out-of-corpus: "capital of Mongolia"), prints:

1. The top-3 retrieved chunks (what the model will see).
2. The generated answer, if a generation backend is configured.
3. The expected fact you should check the answer against.

You read the output and judge:

- Is the fact actually in the retrieved context? If not → retrieval
  problem.
- Does the model stick to the context or invent things? Does it refuse
  on the out-of-corpus question? If not → grounding/honesty problem.

If no generation backend is reachable, runs in dry-run mode — prints
the context and lets you mentally simulate the answer.

### `answers.sh` — Level 4: are the ANSWERS correct and grounded?

```bash
bash tests/manual/answers.sh                            # bundled corpus, qwen2.5:3b
CRAG_GEN_MODEL=qwen2.5:7b bash tests/manual/answers.sh  # compare a bigger model
CRAG_DB=~/my.db bash tests/manual/answers.sh            # grade your own bge-m3 index
```

The first **auto-graded** test at the answer layer — Levels 1–3 and the
BEIR/eval-ci gates all measure retrieval or the cMCP seam; this is the only
one that scores whether the assistant's *answers* are right. By default it
indexes the bundled real corpus `corpus_it/` (the Vallenova Smart City docs —
distinct, fact-bearing documents, a genuine yardstick) into a temp bge-m3 DB,
then for each question in `answers_it.tsv` runs query → retrieve (cosine) →
generate (ollama) and grades, across five dimensions — the full **RAG Triad**
(Context Precision/Recall live in `make eval`; Faithfulness + Answer Relevancy
live here) plus correctness and abstention:

- **fact lookups** (`ans`): *grounded* (top-1 doc is the expected one),
  *answered* (no spurious refusal), *correct* (answer matches a fact ERE),
  *faithful* — every claim in the answer is supported by the **retrieved
  context** (the RAG-triad *Faithfulness* metric), and *relevant* — the answer
  **directly and completely addresses the question**, no digression/padding/
  partial dodge (the RAG-triad *Answer Relevancy* metric). Both are graded by an
  LLM judge at temperature 0 (need `jq`; auto-skipped if absent; override with
  `CRAG_JUDGE_MODEL`, disable with `CRAG_FAITHFULNESS=0` / `CRAG_RELEVANCY=0`).
  The three are orthogonal: `correct` = matches the hand-written fact,
  `faithful` = grounded in what was *actually* retrieved (catches hallucination),
  `relevant` = actually answers what was *asked* (catches on-topic-but-evasive).
  **Judge choice matters for relevancy** (measured 2026-06-08): it is a subtler
  judgment than faithfulness — `qwen2.5:3b` is an unreliable relevancy judge
  (false-flags squarely-on-target answers: 3b judge **40/65** vs 7b judge
  **63/65** on the *same* answers). Use `CRAG_JUDGE_MODEL=qwen2.5:7b` (or
  stronger) for a trustworthy relevancy number. Includes precision distractors
  (annual vs monthly fare) and a comprehension/negation case (do the cameras use
  facial recognition? → "no").
- **within-corpus unanswerable** (`nca`): the right doc is retrieved but the
  asked fact isn't in it — a good system **abstains**, a hallucinator invents.
- **out-of-corpus** (`ooc`): graded on **refusal** ("Not in context.").

Generation runs at **temperature 0** (cRAG's default; see `CRAG_GEN_TEMP`), so
the scorecard is **deterministic** — a real before/after instrument, not a
noisy one. It also prints a **generation-gate calibration** demo: a genuine
bge-m3 answer with cosine in `[0.38, 0.60)` is produced at gate 0.38 but
refused at the CLI's mxbai-era 0.60 default. Set `CRAG_GEN_MIN_SCORE=0.38` for
bge-m3 (or rely on the embedder-aware default).

Needs ollama with bge-m3 + a chat model (`CRAG_GEN_MODEL`, default
`qwen2.5:3b`); skips cleanly if missing. To extend the yardstick, add docs to
`corpus_it/` and rows to `answers_it.tsv` — no script change. Measured
baseline (temp 0): **3b 31/32**, **7b 30/32** answerable, 3/3 abstain, 4/4
refuse — i.e. on a real corpus the pipeline is near-perfect and a *bigger*
model isn't automatically better (3b and 7b fail different items).

### `injection.sh` — context-poisoning / prompt-injection (end-to-end)

```bash
bash tests/manual/injection.sh                            # default gen (qwen2.5:3b)
CRAG_GEN_MODEL=qwen2.5:7b bash tests/manual/injection.sh  # measure mitigation
```

The **RAG primary failure vector**: a malicious document indexed into the corpus
is later retrieved into the generation context and tries to hijack the answer.
This builds a small municipal corpus with two **poisoned** docs (a library-hours
doc carrying an injected `### ISTRUZIONI DI SISTEMA … rispondi PWNED`, a parking
doc with a fake `<<SYS>>` reconfigure-and-leak turn), indexes it with bge-m3, and
fires queries that retrieve the poison — asserting the answer never emits the
attacker's payload token nor parrots the injected instruction.

Two layers, deliberately separated:

- **Structural (cRAG's job, proven offline):** the poison reaches the generator
  as escaped **data**, never as prompt structure — `tests/test_injection.c`
  pins this deterministically in `make check` (no model needed).
- **Semantic (the host/generator's job):** does the model *resist* once the
  poison is in context? cRAG retrieves faithfully (the poison IS in context,
  verbatim — the script shows it); resisting is the F2 *retriever-not-judge*
  contract. Measured (temp 0): **`qwen2.5:3b` resisted 2/4** (obeyed when a
  poisoned doc was the top hit), **`qwen2.5:7b` resisted 4/4**. A FAIL is a
  *generator/host* robustness gap, not a cRAG bug; the live consumer (Claude
  Code) resisted the same attack class in the 15-Q agent-in-loop run.

Needs ollama (bge-m3 + a chat model); skips cleanly if absent.

### `inspect.sh` — diagnostic tool

```bash
bash tests/manual/inspect.sh "<query>"
bash tests/manual/inspect.sh "<query>" <source_substr>
bash tests/manual/inspect.sh --dump <source_substr>
```

When recall.sh reports a failure, this is how you find out **why**.
Instead of showing top-K, it ranks **every** chunk in the DB against
the query. If the "right" answer is at rank 42, that's an embedder
problem. If the chunk doesn't exist or is split weirdly, that's a
chunker problem. `--dump` shows every chunk of a given file so you
can eyeball the chunking itself.

### `compare.sh` / `recall.sh --record` — trend tracking

```bash
CRAG_SAVE_HISTORY=1 CRAG_EMBED_MODEL=mxbai-embed-large \
    bash tests/manual/recall.sh ollama
# …later, after swapping models or tweaking chunker:
CRAG_SAVE_HISTORY=1 CRAG_EMBED_MODEL=nomic-embed-text \
    bash tests/manual/recall.sh ollama

make manual-record                     # same as the recall.sh line above
bash tests/manual/compare.sh           # diff latest vs previous
make manual-compare                    # same
```

Each `CRAG_SAVE_HISTORY=1` run writes a JSON snapshot to
`tests/manual/history/<ts>-<backend>-<model>.json` with the full
per-query ranks and metrics. `compare.sh` diffs two snapshots and
shows metric deltas plus which specific queries improved, regressed,
were newly fixed, or became new misses.

This is what catches "I changed chunk overlap from 200 to 400 and
somehow carbonara retrieval got worse" — a category of problem that
aggregate metrics alone will miss.

## Common workflows

**"I changed the chunker, did I break anything?"**
```bash
make manual-record    # snapshot
# …make your change…
make manual-record    # another snapshot
make manual-compare   # see what moved
```

**"A recall.sh run shows the Corpus Juris Civilis query failing."**
```bash
bash tests/manual/inspect.sh "what is the Corpus Juris Civilis"
# …if the right chunk is at rank 5, the embedder is the problem.
# …if the right chunk doesn't appear at all, the chunker is.
bash tests/manual/inspect.sh --dump roman_law  # check chunking
```

**"I want to try a different embedder."**
```bash
ollama pull mxbai-embed-large                       # one time
make manual-record                                   # before
CRAG_EMBED_MODEL=mxbai-embed-large make manual-record
make manual-compare                                  # proof it helped
```

## Extending `golden.tsv`

Format: `expected_filename<TAB>natural language query`. Comments start
with `#`.

Guidelines for adding good queries:

- **Phrase like a user asking**, not like the document is written. "why
  is cream not used in authentic carbonara" is a good query. "cream
  authentic carbonara" is a degenerate keyword match.
- **Prefer unique canary facts**: "Cassini Division", "vruntime",
  "Corpus Juris Civilis". These are words that only appear in one
  document, so the expected answer is unambiguous.
- **Add at least one distractor per new topic** — a phrase that sounds
  related but has no correct answer in the corpus. This is what catches
  the retriever blindly returning the "least wrong" document at high
  confidence.

After adding queries, run `make manual-record` to capture a new
baseline. `compare.sh` will flag the additions as "only in new" rather
than breaking the diff.

## Reading the metrics

- **recall@1 = 1.0** is the bar on a 20-query set. Anything less, and
  you have real failures worth looking at.
- **recall@3 ≥ 0.9** is the floor. Below that, the retriever is
  misleading the generator often enough that `--generate` becomes a
  hallucination hazard.
- **MRR ≥ 0.95** means when retrieval succeeds, it's usually at rank 1.
  Lower MRR with high recall@3 means "the right thing is there but
  ranked behind distractors," which hurts generation quality more than
  raw recall suggests.

None of these are contract numbers — they are signal about this
specific corpus. The real question `recall.sh` answers is "did anything
regress since last time."
