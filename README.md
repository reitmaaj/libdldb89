# libdldb89

`libdldb89` is a persistent database for finite positive Datalog. It is
assembled from sibling libraries rather than implementing their concerns again:

- `libdlx89` — DLX program interchange (parse, validate, canonical write).
- `libdlq89` — DLQ conjunctive queries against an abstract snapshot interface.
- `libdatalog89` — least-fixed-point evaluation over an abstract store.
- `libunicode89` — strict UTF-8 and UAX #31 identifier handling (via DLX/DLQ).
- SQLite — durable storage, transactions, and indexed scans. Private to this
  library; no SQLite type ever appears in `include/dldb89.h`.

## Model

The value domain is exactly two kinds: exact UTF-8 strings (no normalization)
and arbitrary-precision integers in canonical ASCII. `42` and `"42"` are
distinct values.

A predicate identity is `(NFC identifier, arity)`. Each predicate has:

- an **EDB** table holding explicitly asserted facts; and
- a **materialized** table holding `M = lfp(EDB, Rules)`.

Every committed generation satisfies `M = lfp(EDB, Rules)`. There is no dirty
state: a mutation computes the fixed point and commits it in a single SQLite
write transaction. On failure the previous generation is untouched. Readers
observe either the old or the new complete fixed point, never a partial one.

Rules are persisted as canonical DLX text. Asserted facts are persisted as
interned values in per-relation EDB tables. `dldb89_dump` emits the canonical
DLX of asserted facts and rules (derived facts are not emitted).

## Architecture

```
dldb (CLI) -- libdldb89 --+-- libdlx89
                          +-- libdlq89   (dlq89_db_ops adapter)
                          +-- libdatalog89 (datalog89_store_ops adapter)
                          +-- SQLite
```

Evaluation clears every materialized table, seeds it from the EDB through the
`datalog89` store adapter, installs the persisted rules, and runs the evaluator
to a fixed point. Queries run through `dldb89_query_db`, a `dlq89_db` whose
scans join the materialized table to the interned value table so typed scalars
are returned directly to DLQ.

## Build

Requires SQLite 3 development headers and the sibling static archives.

```sh
just build   # builds siblings, then libdldb89.a
just test    # smoke + rejection/atomicity tests
```

The default build is strict C89: `-std=c89 -pedantic-errors -Wall -Wextra
-Werror`.

## Tests

- `test/test_smoke.c` — create, load facts+rules, recursive query, typed
  integer/string disambiguation, canonical dump, `check --full`, reopen, and
  rule replacement.
- `test/test_reject.c` — invalid DLX is rejected without changing the database;
  `replace_rules` rejects facts; create-over-existing fails; unknown formats and
  non-databases are rejected.
