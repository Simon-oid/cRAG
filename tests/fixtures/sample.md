# Sample Document

This is a sample markdown file used by the cRAG test suite.

## Introduction

cRAG is a small portable C RAG system. It indexes files, embeds their chunks,
and retrieves the most similar chunks for a query.

## Code Example

```c
int main(void) {
    return 0;
}
```

The fence above must survive chunking without being split.

## Conclusion

Short documents fit in a single chunk. Longer documents are split at headers
and, if a section is too large, via a sliding window with overlap.
