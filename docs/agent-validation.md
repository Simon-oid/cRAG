# Agent-in-the-loop validation (Tier 4.3)

Real-agent validation of cRAG through its MCP wrapper (`crag-mcp`, in the
cMCP repo), with **Claude Code as the host**. butlerbot is the eventual
consumer but is unfinished, so per councils #3/#4/#5 the live consumer is
**cMCP + Claude Code today**. Every failure files against the `crag-mcp`
tool description or result format — not the model.

## Config (the bge-m3 Italian deployment)

```sh
# 1. Embedder: Ollama + bge-m3 (multilingual, 1024-dim). cutoff auto-resolves to 0.38.
ollama list | grep bge-m3

# 2. Index the GRADABLE real corpus (NOT the templated rag-test.db).
#    Use a PERSISTENT path, never /tmp — see Finding F1.
cd ~/cRAG
CRAG_EMBED_BACKEND=ollama CRAG_EMBED_MODEL=bge-m3 \
  ./crag index tests/manual/corpus_it --db ~/.crag/crag-it.db --ext md --workers 2
# → 12 files, 12 chunks, dim 1024

# 3. Register crag-mcp with Claude Code (embedder env MUST be propagated)
claude mcp add crag-it \
  --env CRAG_EMBED_BACKEND=ollama \
  --env CRAG_EMBED_MODEL=bge-m3 \
  -- ~/cMCP/tools/crag-mcp/crag-mcp --db ~/.crag/crag-it.db
```

`claude mcp list` → `crag-it ... ✓ Connected`.

> **Note:** MCP servers added mid-session don't expose their tools until
> Claude Code reloads. After `mcp add`, **restart the session** (or open a
> fresh one) for `crag_search` / `crag_stats` to become callable as the agent.

## First-light (2026-06-07) — protocol-level seam proof

Before the agent drives it, the full wire was exercised directly over stdio
(handshake → `tools/list` → `tools/call crag_search`):

- Server log: `crag-mcp: relevance gate cosine >= 0.380 (calibrated for embedder 'bge-m3')`
  — the calibration fix confirmed live (was the deployment-breaking bug).
- `tools/list` exposes `crag_search(query, k?)` and `crag_stats()`, with a
  tool description that already explains the `[cos … bm25 … fusion …] <path>`
  prefix and the `(no chunk cleared the relevance threshold)` refusal string.
- Query *"quali sono gli orari dello sportello al cittadino"* → top hit
  `tests/manual/corpus_it/sportello.md` (cos 0.479), #2 `app_cittadino.md`
  (cos 0.494). Correct doc surfaced, citation prefix intact, gate passed.

The seam answers grounded, citable results under the bge-m3/0.38 config.

## Findings (filed against crag-mcp tool/result, not the model)

### F1 — server serves an empty index silently, and the refusal string lies (2026-06-07)
First real agent-in-the-loop call (`crag_stats`) returned `chunks: 0, files: 0,
dim: 0` while the CLI on the same path showed 12. Two distinct problems:

1. **Operational:** the index lived in `/tmp`, which the OS reaped across a
   restart — file stub left, data pages gone. A long-lived `crag-mcp` process
   also holds the DB handle open, so deleting + re-creating the file leaves the
   server reading a **stale/unlinked inode** (0 chunks) until it reopens
   (= a session reload). **Fix: serve indexes from a persistent path
   (`~/.crag/…`), and never `rm` a DB a running server has open — reindex in
   place or restart the server.**
2. **Server (real cRAG/crag-mcp finding):** `crag_search` on an empty index
   returns `(no chunk cleared the relevance threshold)` — and the tool
   description explicitly tells the agent that string "means the index has no
   strong match for this query — **not** that the index is empty." With a
   0-chunk index that promise is **false**, and the agent has no signal that
   retrieval is structurally broken vs. merely unmatched. **Proposed fix:**
   crag-mcp should detect `chunk_count == 0` and return a distinct, honest
   message (e.g. `(index is empty — 0 chunks)`), or warn loudly at startup.
   Cheap: it already has the stats. Filed against crag-mcp result format.

   **FIXED 2026-06-07** (cMCP `tools/crag-mcp/main.c`, rebuilt green). The
   `n == 0` branch now calls `store_stats` and returns a distinct message:
   empty index → `(index is empty — 0 chunks; check the --db path / ingest,
   this is NOT a no-match result)`; non-empty but gated → `(no chunk cleared
   the relevance threshold — the index is non-empty but nothing matched this
   query)`. Verified both paths over the MCP wire.

### F2 — retrieval gate doesn't abstain on out-of-corpus, same-domain queries (2026-06-07)
The 0.38 cosine gate is tuned for *recall* (admit genuine bge-m3 matches, which
sit at 0.43–0.58). But bge-m3 assigns **0.41–0.57 cosine to completely unrelated
queries** when the whole corpus shares one register (municipal Italian prose).
Measured on 3 out-of-corpus probes — none of which the corpus can answer:

| OOC query | top hit | top cos | gate refused? |
|---|---|---|---|
| orari piscina comunale | wifi.md | 0.47 | no |
| costo loculo al cimitero | trasporti.md | **0.566** | no |
| orari biblioteca / prestito | illuminazione.md | 0.46 | no |

So `crag_search` returns plausible-looking but **wrong** chunks with no
abstention signal — the retrieval gate cannot separate "relevant" from
"same-domain-but-irrelevant." **The refusal burden falls entirely on the
consuming agent/generator.** Verified the generator *does* catch it: full
pipeline (`crag query --generate`, qwen2.5:3b + RAG_SYSTEM) on the cimitero
case (cos 0.566) → **"Not in context."** So **end-to-end with Claude Code as
host is safe** (Claude is the generator and refuses, exactly like qwen did).
The risk is only for a naive host that pipes `crag_search` chunks into a weak
model without a refusal prompt.

**Implications (filed, not yet fixed):**
- A pure cosine threshold is the wrong instrument for abstention on a
  single-domain corpus — precision and recall can't both be served by one
  global cutoff. Candidate signals: relative score gap (top1 vs top2), or a
  cross-encoder/LLM relevance check on the top chunk (cheap at k≤3). NOT a
  reason to raise 0.38 — that would re-break recall (the original calibration bug).
- Document the contract clearly: `crag_search` is a *retriever*, not a
  *gatekeeper*; the host MUST apply its own grounding/refusal. The tool
  description should say so (it currently implies the gate handles relevance).

## Playbook results (2026-06-07, first real agent-in-the-loop run)

Driven as Claude Code calling `crag_search` (k=3) over the bge-m3 `corpus_it`
index. Scored on whether the correct source doc is rank 1.

| Class | Score | Notes |
|---|---|---|
| Factual (5) | **5/5** | correct doc @ rank 1, cos 0.63–0.66 |
| Navigational (5) | **5/5** | correct doc @ rank 1, cos 0.52–0.65 |
| Abstractive (5) | **5/5** | correct doc @ rank 1, cos 0.57–0.72 |
| **Answerable total** | **15/15** | every gold doc retrieved at rank 1 |
| OOC abstention (retrieval layer) | **0/3** | gate admits irrelevant chunks → Finding F2 |
| OOC abstention (end-to-end w/ generator) | 1/1 tested | "Not in context" — generator refuses correctly |

**Citation:** every result carries `[cos … bm25 … fusion …] <source path>` +
chunk body, so the agent can attribute answers to a named source verbatim. ✓

**Verdict:** retrieval is *not* the bottleneck on this corpus (15/15 @1, as the
offline harness predicted). The one real seam-level finding is F2: abstention is
the agent's job, not the gate's.

## F2 resolved — measured, signal killed (2026-06-07, council #6)

Swept four cheap abstention signals over the existing 77 labeled rows
(65 answerable / 6 nca / 6 ooc) via `crag query` hybrid (script: throwaway
`/tmp/f2_sweep.py`). Looking for a single cut that abstains on the 12 negatives
without refusing the 65 answerable:

| signal | answerable range | negatives range | best cut |
|---|---|---|---|
| `fusion_gap` (top1−top2 RRF) | [0.000, 0.002] | [0.001, 0.002] | abstains **0/12** — fully overlapping |
| `cos_top1` | [0.478, 0.729] | [0.224, **0.598**] | keep 65/65, abstain only 6/12 (6 false-accept) |
| `cos_gap` | [0.012, 0.303] | [−0.062, 0.248] | 1 false-refuse + 6 false-accept |
| `rel_cos_gap` | [0.022, 0.465] | [−0.244, 0.415] | 8 false-accept |

**No clean cut exists → arithmetic abstention is dead** (the pre-committed kill
criterion). Two reasons, both measured:
1. `fusion_gap` is constant (~0.001–0.002) regardless of relevance — RRF scores
   are dominated by rank position `1/(60+r)`, not relevance magnitude. (Same
   failure mode that killed `CRAG_GEN_BAND`.)
2. Same-domain negatives reach cos **0.598** (e.g. nca *"how much was spent on
   bike paths alone"* → budget doc @ 0.544) and overlap the answerable floor
   (0.478). Far-OOC (*"capital of Australia"* 0.286, *"football team"* 0.327)
   already falls below the existing 0.38 gate — so only *same-domain*
   unanswerables leak, and those are arithmetically unseparable. Abstention
   there is a semantic "is this specific fact present?" judgment — only the
   generator/agent can make it (and both qwen + Claude do, correctly).

**Action taken (no-regret):** rewrote the `crag_search` tool description
(cMCP `tools/crag-mcp/main.c`) to state the contract explicitly — it is a
*retriever, not a relevance judge*; the host MUST read the chunks and abstain if
the asked-for fact is absent; the coarse gate only drops far-off-topic matches.
No abstention signal added to the library (measured useless); no raising of
0.38 (would re-break recall). F2 is **answered, not postponed.**

## Item D — cross-doc / multi-hop probe (2026-06-07, council #6)

Built `tests/manual/answers_it_multihop.tsv`: 8 questions each genuinely
requiring ≥2 distinct docs (built from the corpus's shared entities: ZTL×EV-
charging, LED×budget, air-quality×transport, video×wifi, etc.). Measured
retrieval coverage (script `/tmp/multihop_coverage.py`) AND end-to-end synthesis.

- **Retrieval coverage: 8/8 BOTH docs surfaced at k=3** — but *trivially*, and
  this is the finding the council's peer review predicted: every question names
  both topics, so both docs rank independently on their own terms. The 12
  single-chunk docs **cannot pose a true multi-hop *retrieval* challenge** (fetch
  doc B *because* doc A is relevant); cRAG does single-shot top-k and has no
  iterative/graph retrieval. So multi-hop *retrieval* is untestable on this
  corpus, not passed.
- **Synthesis of additive facts works** (qwen2.5:3b): e.g. "−62% + efficienza
  energetica 4 mln" ✓, "rifiuti 167 € + ZTL 83 €" ✓.
- **Cross-doc *reasoning* is the real gap, and it's model-robust.** Q1 ("with an
  *electric* car, charging cost + ZTL entry cost"): both docs retrieved, and
  `ztl.md` (in context) says electric vehicles are **exempt** from the ZTL fee —
  yet **both 3b AND 7b** report the generic 83 € fine, failing the reasoning hop
  "electric → exempt → 83 doesn't apply." The facts were present; neither model
  *applied* the conditional under the strict RAG prompt.

**Q1 promptability probe (2026-06-07):** tested whether the cross-doc reasoning
miss is fixable via `RAG_SYSTEM`. Added a rule — "apply the question's qualifiers
(vehicle/eligibility) to conditional rules/exemptions; answer for that specific
case" + an anti-embellishment guard — rebuilt, and re-ran the full graded harness
(`answers.sh`, temp 0). Result: **scorecard IDENTICAL to baseline** (correct
64/65, abstain 6/6, refuse 6/6 — no regression, no gain). In isolation (both full
docs as context) the rule recovers the EV exemption but embellishes a wrong fact
("residents' pass free" — it's 25€); in the *real* retrieval pipeline it flips Q1
from a confident wrong answer (83€) to a full refusal — safer (false-answer >
false-refusal) but still not *correct*, and it drops the answerable charging fact
too. **Reverted** (no graded-metric movement → not worth the prompt complexity;
measure-before-keep). Conclusion: the cross-doc reasoning miss is a genuine
small-model limit, not a prompt defect; the lever is a bigger model or
**agent-side decomposition** (Claude-as-host can issue the sub-questions as
separate `crag_search` calls) — not library prompt-tuning.

**D verdict:** consistent with the program's standing conclusion — retrieval is
not the bottleneck; **generation/reasoning is.** This corpus can probe cross-doc
*reasoning* (and just exposed a real, size-robust miss) but cannot probe
multi-hop *retrieval*. A genuine multi-hop *retrieval* benchmark would need facts
deliberately **split across linked docs** so doc B is only findable via doc A —
the next substrate to build if/when multi-hop retrieval becomes a real need
(it isn't yet: the live host is Claude, which can issue its own follow-up
`crag_search` calls — agent-driven iterative retrieval beats a graph index for
this consumer).

## Full 15-Q playbook — Claude-as-host, end-to-end (2026-06-08)

The 4.3 first-light run (above) scored *retrieval rank* with Claude calling
`crag_search` but did not exercise Claude's own **abstention and reasoning** as
the host generator. After the Tier-4 hardening backlog closed (4.4/4.7/4.8/4.5),
the loop returned to Council #5's directive — drive the real consumer — so this
run expands first-light to the full graded playbook with **Claude Code itself as
the reasoning host** (not qwen): retrieve → read chunks → decide answer / abstain
/ refuse, graded against `answers_it.tsv` + `answers_it_multihop.tsv`.

15 questions spanning every dimension, k=3 (k=4 for the cross-doc one):

| # | Type | Top-1 (cos) | Host decision | ✓ |
|---|---|---|---|---|
| 1 | fact — ZTL fine | ztl 0.657 | 83 € | ✓ |
| 2 | fact — recycling % | rifiuti 0.581 | 74 % | ✓ |
| 3 | fact — single bus ticket | trasporti 0.609 | 1,50 € | ✓ |
| 4 | fact — total budget | bilancio 0.622 | 14,2 M€ | ✓ |
| 5 | fact — fast-charger kW | ricarica_ev 0.698 | 50 kW | ✓ |
| 6 | **cross-doc reasoning** — EV cost to enter ZTL | ztl 0.575 + ev 0.523 | **exempt, no fine** | ✓ |
| 7 | **nca (F2)** — total ZTL fines 2025 | ztl 0.492 | **abstain** | ✓ |
| 8 | **nca (F2)** — ozone daily threshold | aria 0.603 | **abstain** (PM2.5 = distractor) | ✓ |
| 9 | **nca (F2)** — exact bike-lane spend | bilancio 0.553 | **abstain** | ✓ |
| 10 | ooc — pizza recipe | *(gate dropped)* | refuse | ✓ |
| 11 | ooc — nginx config | *(gate dropped)* | refuse | ✓ |
| 12 | precision — **annual** bus pass | trasporti 0.628 | 310 € (not the 35 € monthly) | ✓ |
| 13 | negation — facial recognition? | videosorv. 0.634 | **no, not used** | ✓ |
| 14 | **nca (F2)** — lamps failed 2025 | illumin. 0.570 | **abstain** | ✓ |
| 15 | fact — trees in public green | verde 0.612 | 6.200 | ✓ |

**Score: 15/15.** No failure to file against the tool description or result
format. What this validates that the qwen harness could not:

1. **The cross-doc reasoning miss (Q1/item D) is a small-model limit, not a
   system limit — now demonstrated.** The EV-exemption fact is inline in
   `ztl.md` and was retrieved at top-1; qwen 3b *and* 7b both failed to apply
   *"esentati … i veicoli completamente elettrici"* and reported the generic
   83 €. Claude-as-host reads the clause and answers **exempt**. This is the live
   confirmation of the D/Q1 prediction: the lever was reasoning capability. The
   capable host needed no decomposition — a single semantic query surfaced the
   exempting chunk.
2. **The F2 "retriever-not-judge" contract works *because the host honors it*.**
   All three `nca` cases returned a plausible in-domain chunk at cos **0.49–0.60**
   — above the 0.38 gate, exactly the same-domain leak the F2 sweep proved
   un-gateable with arithmetic. The host read each chunk, found the specific fact
   absent, and abstained. The contract rewrite (the F2 no-regret deliverable) is
   doing its job at the seam.
3. **The 0.38 bge-m3 gate is correctly calibrated in practice.** Far-ooc (pizza,
   nginx) dropped by the gate; in-domain-but-unanswerable passed it (correctly —
   they *are* in-domain) and were left to host judgment; every genuine answerable
   surfaced at top-1. Zero false gates in either direction across 15 queries.
4. **The result format carries grounding + citation.** `[cos bm25 fusion] <path>`
   + verbatim chunk was sufficient to attribute every answer to a named source
   and to make the abstain/answer decision. Nothing more was needed.

**Verdict:** end-to-end, with the real intended host (a capable agent), the
bge-m3 seam delivers correct, grounded, citable answers *and* correct abstention
across the full quality gradient — including the one case (cross-doc EV exemption)
that defeated both local generation models. The standing conclusion holds and is
now demonstrated rather than inferred: retrieval is at ceiling; correctness lives
in the host's reasoning + the abstention contract, both of which work. Tier 4.3
(agent-in-the-loop validation) is **closed**.
