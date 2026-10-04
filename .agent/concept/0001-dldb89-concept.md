# libdldb89 concept

`libdldb89` is a persistent finite-positive-Datalog database assembled from
independently useful sibling libraries rather than a monolith. SQLite is only
the persistence, transaction, and indexed-scan mechanism; it contributes no
Datalog semantics.

- Program interchange: `libdlx89` (typed string/integer), canonical.
- Queries: `libdlq89` against a snapshot/scan adapter.
- Inference: `libdatalog89` over an insert/scan store adapter.
- Identifiers/UTF-8: `libunicode89` via DLX/DLQ.

The product goal is a small, inspectable, production-aiming database whose
derived state is always a complete least fixed point.
