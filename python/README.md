# crag-index

cRAG's retrieval index as a Python library: text chunks with their
embedding vectors in one SQLite file, searched by exact cosine, BM25
(SQLite FTS5) or both fused with Reciprocal Rank Fusion. The engine is
cRAG's C code compiled into the package — no runtime dependency beyond
`cffi`. Bring your own embedder.

```python
import numpy as np, crag

with crag.Index("notes.db") as idx:
    idx.add_many([(src, text, vec) for src, text, vec in my_chunks])
    for hit in idx.search(vector=query_vec, text="query words", k=5):
        print(hit.source, hit.score, hit.cosine)
```

Vectors must be 1-D C-contiguous float32 (NumPy `float32` or
`array.array("f")`); they reach C without a copy. See the cRAG README for
what each search mode measures and where it fails.
