# Security policy

## Supported versions

cRAG is pre-1.0. Fixes land on `main` and ship in the next release; older
tags are not back-patched.

## Reporting a vulnerability

Please report vulnerabilities privately via
[GitHub's private vulnerability reporting](https://github.com/Simon-oid/cRAG/security/advisories/new)
(Security tab → "Report a vulnerability"). Do not open a public issue for
an exploitable bug. You should get an acknowledgement within a few days; a
minimal reproducer is the fastest to act on.

## Scope

The parser and indexer read untrusted files (chunker fuzzed by `test_fuzz`); the generation prompt fences retrieved text against prompt injection. See `KNOWN_LIMITATIONS.md` for what is not defended.
