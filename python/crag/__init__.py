"""crag — cRAG's hybrid retrieval index for Python.

An on-disk index of text chunks with their embedding vectors, searched by
exact cosine, BM25 (SQLite FTS5) or both fused with Reciprocal Rank
Fusion. You bring the vectors (any embedder); the index stores, ranks and
returns them. The engine is cRAG's C code, compiled into this package.

    import numpy as np, crag

    with crag.Index("notes.db") as idx:
        idx.add("a.md", "the quick brown fox", np.array([1, 0, 0], np.float32))
        for hit in idx.search(vector=q, text="fox", k=5):   # hybrid
            print(hit.source, hit.score, hit.cosine)

Vectors are passed to C without a copy (`ffi.from_buffer`), so they must
be C-contiguous float32: a NumPy array of dtype float32, or
`array.array("f", ...)`. Anything else raises TypeError instead of being
silently converted.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from ._native import ffi, lib

__all__ = ["Index", "Hit", "CragError", "COSINE", "BM25", "HYBRID"]
__version__ = "0.2.0"

COSINE = "cosine"
BM25 = "bm25"
HYBRID = "hybrid"
_MODES = {COSINE: lib.CRAG_MODE_COSINE, BM25: lib.CRAG_MODE_BM25,
          HYBRID: lib.CRAG_MODE_HYBRID}


class CragError(RuntimeError):
    """A negative return code from the C library."""

    def __init__(self, code: int, what: str):
        self.code = code
        super().__init__(f"{what}: {ffi.string(lib.crag_errstr(code)).decode()} ({code})")


@dataclass(frozen=True)
class Hit:
    id: int
    source: str
    text: str
    score: float    # ranking score of the mode (RRF fusion score for hybrid)
    cosine: float   # cosine similarity, or -2.0 when not computed (bm25 mode)
    bm25: float     # BM25 score, higher is better; 0.0 without a lexical match


def _check(code: int, what: str) -> None:
    if code != 0:
        raise CragError(code, what)


def _vector(vec: Any) -> tuple[Any, int]:
    """A zero-copy float* view of `vec` and its length."""
    try:
        view = memoryview(vec)
    except TypeError:
        raise TypeError("vector must support the buffer protocol "
                        "(NumPy float32 array or array.array('f'))") from None
    if view.format not in ("f", "<f", "=f") or view.ndim != 1 or not view.c_contiguous:
        raise TypeError(f"vector must be a 1-D C-contiguous float32 buffer, "
                        f"got format {view.format!r}, ndim {view.ndim}")
    return ffi.from_buffer("float[]", vec), view.shape[0]


class Index:
    """An index file (or ":memory:"). Use as a context manager, or close()."""

    def __init__(self, path: str = ":memory:"):
        self._db = None
        out = ffi.new("crag_db_t **")
        _check(lib.crag_db_open(path.encode(), out), f"open {path!r}")
        self._db = out[0]

    def close(self) -> None:
        if self._db is not None:
            lib.crag_db_close(self._db)
            self._db = None

    def __enter__(self) -> "Index":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()

    def _handle(self) -> Any:
        if self._db is None:
            raise ValueError("index is closed")
        return self._db

    def add(self, source: str, text: str, vector: Any) -> None:
        """Add one chunk. All chunks of an index share one vector length."""
        buf, dim = _vector(vector)
        _check(lib.crag_db_add(self._handle(), source.encode(), text.encode(),
                               buf, dim), "add")

    def add_many(self, items: Any) -> int:
        """Add (source, text, vector) triples in one transaction."""
        db = self._handle()
        _check(lib.crag_db_begin(db), "begin")
        n = 0
        try:
            for source, text, vector in items:
                self.add(source, text, vector)
                n += 1
        finally:
            _check(lib.crag_db_commit(db), "commit")
        return n

    def search(self, vector: Any = None, text: str | None = None, k: int = 10,
               mode: str | None = None) -> list[Hit]:
        """Top-k hits, best first.

        mode defaults to hybrid when both vector and text are given, else to
        cosine (vector only) or bm25 (text only)."""
        if mode is None:
            mode = HYBRID if vector is not None and text is not None else (
                COSINE if vector is not None else BM25)
        if mode not in _MODES:
            raise ValueError(f"mode must be one of {sorted(_MODES)}")
        qbuf, qdim = (_vector(vector) if vector is not None else (ffi.NULL, 0))
        qtext = text.encode() if text is not None else ffi.NULL
        out_hits = ffi.new("crag_hit_t **")
        out_n = ffi.new("int *")
        _check(lib.crag_db_search(self._handle(), _MODES[mode], qbuf, qdim,
                                  qtext, k, out_hits, out_n), f"search ({mode})")
        try:
            return [Hit(int(h.id), ffi.string(h.source).decode(),
                        ffi.string(h.text).decode(), float(h.score),
                        float(h.cosine), float(h.bm25))
                    for h in (out_hits[0][i] for i in range(out_n[0]))]
        finally:
            lib.crag_hits_free(out_hits[0], out_n[0])

    def stats(self) -> dict[str, int]:
        """{"chunks", "sources", "dim"} of the index."""
        c, s, d = ffi.new("int *"), ffi.new("int *"), ffi.new("int *")
        _check(lib.crag_db_stats(self._handle(), c, s, d), "stats")
        return {"chunks": c[0], "sources": s[0], "dim": d[0]}
