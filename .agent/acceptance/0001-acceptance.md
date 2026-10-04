# Acceptance

## Must exhibit

- Loading a valid DLX program commits `M = lfp(EDB, Rules)` atomically.
- Recursive and mutually recursive rules reach the least fixed point.
- `42` and `"42"` are distinct; both round-trip through dump.
- `dldb89_dump` is canonical and excludes derived facts.
- Data and generation survive close/reopen.
- `dldb89_replace_rules` preserves EDB and recomputes derived state.
- `dldb89_check(full)` passes for a consistent database.

## Must reject

- Invalid DLX: `DLDB89_EPROGRAM`; database and generation unchanged.
- `replace_rules` with a fact clause: `DLDB89_EPROGRAM`; unchanged.
- `dldb89_create` on an existing path: `DLDB89_EDATABASE`, no handle.
- Unknown `dldb_meta.format`: `DLDB89_EFORMAT`, no handle.
- Non-database file: open fails cleanly, no handle.
- Querying a relation absent from the catalog: DLQ reports `unknown-relation`.
