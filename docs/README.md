# Documentation

This directory holds the measured, deep-dive documentation for cRAG. For the
project overview, build instructions, and quickstart, start at the
[root README](../README.md).

## Index

| Document | What's in it |
|---|---|
| [`benchmarks.md`](benchmarks.md) | Human-verifiable measurements: indexing throughput, query latency, retrieval spot-checks, the embedder language probe (mxbai vs bge-m3), and BEIR/NFCorpus evals. Reproduce locally before quoting. |
| [`agentic-readiness.md`](agentic-readiness.md) | The plan for getting cRAG to a quality bar where an LLM agent can rely on its retrieval results without a human in the loop. |
| [`agent-validation.md`](agent-validation.md) | Agent-in-the-loop validation through the MCP wrapper with a real agent host — the end-to-end "first light" results and the multi-hop reasoning findings. |
| [`crag-performance-report.pdf`](crag-performance-report.pdf) | A consolidated performance report, including the same-embedder parity framing for the BEIR comparison and the operating-envelope analysis. |

## Related reading

- [`../ARCHITECTURE.md`](../ARCHITECTURE.md) — module-by-module design, data
  flow, design constraints, and the full CLI reference.
- [`../ROADMAP.md`](../ROADMAP.md) — what shipped, what's deferred, and why.

## A note on the numbers

Benchmarks were taken on the author's machine (Ryzen 5800X / RX 7800 XT 16 GB)
with a local Ollama embedder. They are load-bearing only in the sense of "this
is what we saw when the harness was run." The offline regression gate
(`make eval-ci`) is what actually guards against regressions in CI; the
absolute-quality numbers (`make eval`, BEIR) require a live embedder and are
meant to be reproduced, not quoted blind.
