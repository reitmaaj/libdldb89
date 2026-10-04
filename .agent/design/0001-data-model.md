# Frozen dldb data model

- Logic: finite positive Datalog, set semantics, least fixed point.
- Values: exact UTF-8 string, or canonical arbitrary-precision integer ASCII
  (`0`, `-?[1-9][0-9]*`, no `+`, no leading zeros, no `-0`). Kinds never
  compare equal.
- Predicate identity: `(NFC identifier, arity)`.
- EDB: explicitly asserted facts, persistent.
- Rules: positive, safe; identity is canonical DLX; persisted as canonical DLX.
- Materialization: `M = lfp(EDB, Rules)`.
- Every committed generation satisfies the invariant; no dirty state.
- Default load is additive (facts union, rules union).
- Rule replacement keeps EDB and replaces the whole rule set.
- Default export is asserted facts plus rules; derived facts are excluded.
- Query visibility is `M`, snapshot-consistent at one generation.

See `DESIGN.md` for the storage realization and `0002-sqlite-schema.md`.
