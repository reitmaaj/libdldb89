# libdldb89 design notes

## Invariant

For every committed generation `G`:

```text
M(G) = lfp(EDB(G), Rules(G))
```

`M` is always a complete fixed point. There is no dirty committed state.

## Commit algorithm

```text
BEGIN IMMEDIATE
  apply catalog / EDB / rule mutations
  recompute dldb_relation.derived from rule heads
  clear every materialized table (and materialized nullary flags)
  create a datalog89 evaluator over the store adapter
  seed EDB facts into the materialized tables
  load stored rules, translate, install
  datalog89_eval_run()
  generation += 1
COMMIT
```

Any failure rolls the transaction back, leaving the previous generation.

## SQLite schema (private)

```sql
dldb_meta(key TEXT PRIMARY KEY, value BLOB NOT NULL) WITHOUT ROWID
dldb_relation(id INTEGER PRIMARY KEY, name BLOB, arity INTEGER,
              derived INTEGER, UNIQUE(name, arity))
dldb_value(id INTEGER PRIMARY KEY, kind INTEGER, value BLOB,
           UNIQUE(kind, value))
dldb_rule(id INTEGER PRIMARY KEY, head_relation INTEGER,
          dlx BLOB UNIQUE)
dldb_nullary(relation INTEGER, which INTEGER, PRIMARY KEY(relation, which))
```

For each relation of arity `n >= 1`:

```sql
dldb_e<id>(c0..c<n-1> INTEGER NOT NULL, PRIMARY KEY(c0..c<n-1>)) WITHOUT ROWID
dldb_m<id>(c0..c<n-1> INTEGER NOT NULL, PRIMARY KEY(c0..c<n-1>)) WITHOUT ROWID
dldb_m<id>_i<k>(c<k>)          -- one per non-leading column
```

Tuple components are `dldb_value.id` references. Zero-arity relations use
`dldb_nullary` instead of physical tuple tables.

## Adapters

The `datalog89_store_ops` adapter maps `insert` to `INSERT OR IGNORE` into the
materialized table and `scan_*` to SQL over it. The `dlq89_db_ops` adapter opens
a deferred read transaction per snapshot, resolves `(name, arity)` in the
catalog, and scans the materialized table joined to `dldb_value` so each DLQ
scalar carries its kind.

## Dump and check

`dldb89_dump` renders asserted facts through `dlx89_write_clause` and emits
stored rule text, sorting all clauses by canonical bytes. `dldb89_check`
verifies SQLite integrity, format, value canonicality, rule parseability, and
physical-table existence; `full` additionally rebuilds in a rolled-back
transaction and requires the materialization checksum to be unchanged.

## Deferred

- Incremental additive fixed point (currently full rebuild each commit).
- `unicode89_normalize_is_nfc` (DLX/DLQ already guarantee NFC identifiers).
- Shadow-generation double buffering for shorter write-lock windows; the
  single write transaction already provides atomic publication.
