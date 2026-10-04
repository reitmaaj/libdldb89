# SQLite schema

Private to `libdldb89`.

Catalog and metadata:

```sql
dldb_meta(key TEXT PRIMARY KEY, value BLOB NOT NULL) WITHOUT ROWID
dldb_relation(id INTEGER PRIMARY KEY, name BLOB, arity INTEGER,
              derived INTEGER, UNIQUE(name, arity))
dldb_value(id INTEGER PRIMARY KEY, kind INTEGER, value BLOB,
           UNIQUE(kind, value))
dldb_rule(id INTEGER PRIMARY KEY, head_relation INTEGER, dlx BLOB UNIQUE)
dldb_nullary(relation INTEGER, which INTEGER, PRIMARY KEY(relation, which))
```

Per relation (arity `n >= 1`):

```sql
dldb_e<id>(c0..c<n-1> INTEGER NOT NULL, PRIMARY KEY(c0..c<n-1>)) WITHOUT ROWID
dldb_m<id>(c0..c<n-1> INTEGER NOT NULL, PRIMARY KEY(c0..c<n-1>)) WITHOUT ROWID
CREATE INDEX dldb_m<id>_i<k> ON dldb_m<id>(c<k>)   -- k = 1..n-1
```

Tuple columns reference `dldb_value.id`. `c0` is leading; non-leading columns
get single-column indexes so any single bound position has an indexed lookup.
No foreign keys on tuple columns. `dldb_meta.format` is `1`.
