#!/usr/bin/env python3
"""End-to-end tests of the crag CLI against fake model servers.

    python3 tests/test_cli.py [path/to/crag]      # or: make test-cli

The unit tests cover the store, chunker and library; this covers the code
between them and the network: `crag index`, `query` in every search mode,
`--generate` through each generation backend, `export`/`import`, `stats`,
and each embedding backend's wire format. The servers speak the request and
response shapes of llama.cpp, Ollama, OpenAI and Anthropic, and embed text
deterministically (hashed bag of words), so a query finds the document that
shares its words. Standard library only, no network beyond localhost.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CRAG = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "crag"
DIM = 256
ANSWER = "The fake model answers from the context."

DOCS = {
    "fusion.md": "# Fusion\n\nReciprocal rank fusion merges the cosine and BM25 rankings "
                 "with the constant k equal to sixty.\n",
    "commit.md": "# Commits\n\nThe indexer commits to SQLite every twenty files, so a "
                 "crash loses little work.\n",
    "notes.txt": "Plain text notes about tokenizers: unicode61 has no stemming.\n",
    "skip.json": "{\"not\": \"indexed, wrong extension\"}\n",
}


def embed(text: str) -> list[float]:
    vec = [0.0] * DIM
    for word in re.findall(r"[a-z0-9]+", text.lower()):
        h = int.from_bytes(hashlib.sha256(word.encode()).digest()[:4], "little")
        vec[h % DIM] += 1.0
    norm = math.sqrt(sum(x * x for x in vec)) or 1.0
    return [x / norm for x in vec]


class Fake(BaseHTTPRequestHandler):
    """llama.cpp, Ollama, OpenAI and Anthropic shapes, on one port."""

    calls: list[str] = []

    def log_message(self, *args) -> None:       # keep the test output clean
        pass

    def do_POST(self) -> None:
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])) or b"{}")
        Fake.calls.append(self.path)
        if self.path == "/embedding":                       # llama.cpp
            out: dict = {"embedding": embed(body["content"])}
        elif self.path == "/api/embeddings":                # Ollama
            out = {"embedding": embed(body["prompt"])}
        elif self.path == "/v1/embeddings":                 # OpenAI
            texts = body["input"] if isinstance(body["input"], list) else [body["input"]]
            out = {"data": [{"index": i, "embedding": embed(t)} for i, t in enumerate(texts)]}
        elif self.path == "/completion":                    # llama.cpp generation
            out = {"content": ANSWER}
        elif self.path == "/api/generate":                  # Ollama generation
            out = {"response": ANSWER}
        elif self.path == "/v1/messages":                   # Anthropic
            out = {"content": [{"type": "text", "text": ANSWER}]}
        else:
            self.send_error(404)
            return
        data = json.dumps(out).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class CliTest(unittest.TestCase):
    server: ThreadingHTTPServer
    base: str

    @classmethod
    def setUpClass(cls) -> None:
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Fake)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.corpus = self.dir / "corpus"
        self.corpus.mkdir()
        for name, text in DOCS.items():
            (self.corpus / name).write_text(text)
        self.db = str(self.dir / "test.db")
        Fake.calls.clear()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def crag(self, *args: str, backend: str = "local", ok: bool = True,
             **env: str) -> subprocess.CompletedProcess:
        urls = {"local": "/embedding", "ollama": "/api/embeddings", "openai": "/v1/embeddings"}
        full = {k: v for k, v in os.environ.items() if not k.startswith("CRAG_")}
        full.update(CRAG_DB=self.db, CRAG_EMBED_BACKEND=backend,
                    CRAG_EMBED_URL=self.base + urls[backend], OPENAI_API_KEY="test",
                    CRAG_GEN_BACKEND="local", CRAG_GEN_URL=self.base + "/completion",
                    CRAG_GEN_MIN_SCORE="0")
        full.update(env)
        p = subprocess.run([str(CRAG), *args], capture_output=True, text=True, env=full,
                           timeout=60)
        if ok:
            self.assertEqual(p.returncode, 0, f"crag {' '.join(args)}\n{p.stderr}")
        return p

    def query_json(self, text: str, *args: str) -> list[dict]:
        out = self.crag("query", text, "--format", "json", *args).stdout
        data = json.loads(out)
        return data["results"] if isinstance(data, dict) else data

    def index(self, backend: str = "local") -> None:
        self.crag("index", str(self.corpus), backend=backend)

    def test_index_stores_every_text_file_and_skips_other_extensions(self):
        self.index()
        stats = self.crag("stats").stdout
        self.assertRegex(stats, r"files\D*3")
        self.assertRegex(stats, rf"dim\D*{DIM}")

    def test_query_finds_the_document_in_every_search_mode(self):
        self.index()
        for mode in ("cosine", "bm25", "hybrid"):
            with self.subTest(mode=mode):
                hits = self.query_json("how often does the indexer commit to sqlite",
                                       "--search", mode, "-k", "2")
                self.assertTrue(hits, mode)
                self.assertIn("commit.md", json.dumps(hits[0]))

    def test_text_output_names_the_source(self):
        self.index()
        out = self.crag("query", "reciprocal rank fusion constant").stdout
        self.assertIn("fusion.md", out)

    def test_reindexing_unchanged_files_embeds_nothing(self):
        self.index()
        Fake.calls.clear()
        self.index()
        self.assertEqual(Fake.calls, [])
        self.crag("reindex", str(self.corpus))
        self.assertTrue(Fake.calls)

    def test_generate_through_each_backend(self):
        self.index()
        for backend, path in (("local", "/completion"), ("ollama", "/api/generate"),
                              ("anthropic", "/v1/messages")):
            with self.subTest(backend=backend):
                out = self.crag("query", "reciprocal rank fusion", "--generate",
                                CRAG_GEN_BACKEND=backend, CRAG_GEN_URL=self.base + path,
                                ANTHROPIC_API_KEY="test").stdout
                self.assertIn(ANSWER, out)

    def test_generate_refuses_below_the_confidence_gate(self):
        self.index()
        Fake.calls.clear()
        self.crag("query", "zzz qqq unrelated", "--generate", "--search", "cosine",
                  CRAG_GEN_MIN_SCORE="0.99", ok=False)
        self.assertNotIn("/completion", Fake.calls)

    def test_export_then_import_rebuilds_the_index_elsewhere(self):
        self.index()
        dump = self.dir / "dump.jsonl"
        self.crag("export", "--out", str(dump))
        rows = [json.loads(line) for line in dump.read_text().splitlines()]
        self.assertTrue(rows and all({"source", "text"} <= r.keys() for r in rows))
        other = str(self.dir / "other.db")
        self.crag("import", str(dump), "--db", other)
        hits = self.query_json("twenty files crash", "--db", other, "-k", "1")
        self.assertIn("commit.md", json.dumps(hits[0]))

    def test_each_embedding_backend_speaks_its_wire_format(self):
        for backend, path in (("ollama", "/api/embeddings"), ("openai", "/v1/embeddings")):
            with self.subTest(backend=backend):
                Path(self.db).unlink(missing_ok=True)
                Fake.calls.clear()
                self.index(backend)
                self.assertIn(path, Fake.calls)
                hits = self.query_json("unicode61 stemming", "-k", "1")
                self.assertIn("notes.txt", json.dumps(hits[0]))

    def test_a_deleted_file_is_swept_on_the_next_index(self):
        self.index()
        (self.corpus / "notes.txt").unlink()
        self.index()
        self.assertRegex(self.crag("stats").stdout, r"files\D*2")

    def test_ext_filter_and_known_docs(self):
        (self.corpus / "README").write_text("Readme without an extension about fusion.\n")
        self.crag("index", str(self.corpus), "--ext", "md")
        self.assertRegex(self.crag("stats").stdout, r"files\D*2")
        self.crag("index", str(self.corpus), "--ext", "md", "--include-known-docs")
        self.assertRegex(self.crag("stats").stdout, r"files\D*3")

    def test_parallel_workers_store_the_same_chunks(self):
        self.crag("index", str(self.corpus), "--workers", "4", backend="ollama")
        hits = self.query_json("twenty files crash", "-k", "1")
        self.assertIn("commit.md", json.dumps(hits[0]))

    def test_import_of_a_malformed_dump_fails(self):
        bad = self.dir / "bad.jsonl"
        bad.write_text("not json\n{\"source\": \"x\"}\n")
        p = self.crag("import", str(bad), ok=False)
        self.assertNotEqual(p.returncode, 0)

    def test_unknown_command_and_no_arguments_print_usage(self):
        self.assertIn("usage", self.crag(ok=False).stderr)
        self.assertNotEqual(self.crag("frobnicate", ok=False).returncode, 0)
        self.assertIn("0.2.0", self.crag("--version").stdout + self.crag("--version").stderr)

    def test_an_unreachable_embedder_is_an_error_not_an_empty_index(self):
        p = self.crag("index", str(self.corpus), ok=False,
                      CRAG_EMBED_URL="http://127.0.0.1:9/embedding")
        self.assertNotEqual(p.returncode, 0)


if __name__ == "__main__":
    if not CRAG.exists():
        sys.exit(f"{CRAG} not built (run make)")
    unittest.main(argv=sys.argv[:1], verbosity=2)
