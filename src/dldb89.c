/* dldb89.c - lifecycle, errors, SQLite schema, value interning, catalog. */

#include "dldb89_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- Status and options -------------------------------------------------- */

void dldb89_options_init(dldb89_options *options)
{
    if (options == NULL)
    {
        return;
    }
    options->max_arity = 0;
    options->synchronous_full = 1;
}

const char *dldb89_status_name(dldb89_status status)
{
    switch (status)
    {
    case DLDB89_OK: return "OK";
    case DLDB89_END: return "END";
    case DLDB89_ENOTFOUND: return "ENOTFOUND";
    case DLDB89_EINVAL: return "EINVAL";
    case DLDB89_ENOMEM: return "ENOMEM";
    case DLDB89_EIO: return "EIO";
    case DLDB89_EDATABASE: return "EDATABASE";
    case DLDB89_EPROGRAM: return "EPROGRAM";
    case DLDB89_EQUERY: return "EQUERY";
    case DLDB89_EBUSY: return "EBUSY";
    case DLDB89_EFORMAT: return "EFORMAT";
    case DLDB89_ECORRUPT: return "ECORRUPT";
    case DLDB89_EINTERNAL: return "EINTERNAL";
    default: return "UNKNOWN";
    }
}

const char *dldb89_status_message(dldb89_status status)
{
    switch (status)
    {
    case DLDB89_OK: return "success";
    case DLDB89_END: return "end of iteration";
    case DLDB89_ENOTFOUND: return "not found";
    case DLDB89_EINVAL: return "invalid argument";
    case DLDB89_ENOMEM: return "allocation failed";
    case DLDB89_EIO: return "I/O failure";
    case DLDB89_EDATABASE: return "database operation failed";
    case DLDB89_EPROGRAM: return "invalid Datalog program";
    case DLDB89_EQUERY: return "query failed";
    case DLDB89_EBUSY: return "database is busy";
    case DLDB89_EFORMAT: return "incompatible database format";
    case DLDB89_ECORRUPT: return "database is corrupt";
    case DLDB89_EINTERNAL: return "internal invariant failed";
    default: return "unknown status";
    }
}

/* ---- Errors -------------------------------------------------------------- */

void dldb89_clear_error(dldb89 *db)
{
    db->error.status = DLDB89_OK;
    db->error.system_error = 0;
    db->error.sub_status = 0;
    db->error.message = "";
    db->error_buf[0] = '\0';
}

dldb89_status dldb89_fail(dldb89 *db, dldb89_status status, int sub_status,
                          const char *message)
{
    size_t n;
    const char *src;

    db->error.status = status;
    db->error.sub_status = sub_status;
    src = message != NULL ? message : dldb89_status_message(status);
    n = strlen(src);
    if (n >= (size_t)DLDB89_ERRBUF)
    {
        n = (size_t)DLDB89_ERRBUF - 1;
    }
    memcpy(db->error_buf, src, n);
    db->error_buf[n] = '\0';
    db->error.message = db->error_buf;
    return status;
}

dldb89_status dldb89_sql_fail(dldb89 *db, int rc, const char *what)
{
    char buf[DLDB89_ERRBUF];
    const char *msg;

    msg = sqlite3_errmsg(db->sql);
    if (msg == NULL)
    {
        msg = "sqlite error";
    }
    sqlite3_snprintf((int)sizeof(buf), buf, "%s: %s", what, msg);
    db->error.system_error = rc;
    return dldb89_fail(db, DLDB89_EDATABASE, rc, buf);
}

/* ---- SQL helpers --------------------------------------------------------- */

dldb89_status dldb89_exec(dldb89 *db, const char *sql)
{
    char *err;
    int rc;

    err = NULL;
    rc = sqlite3_exec(db->sql, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK)
    {
        char buf[DLDB89_ERRBUF];
        sqlite3_snprintf((int)sizeof(buf), buf, "%s",
                         err != NULL ? err : "sqlite error");
        sqlite3_free(err);
        db->error.system_error = rc;
        return dldb89_fail(db, DLDB89_EDATABASE, rc, buf);
    }
    sqlite3_free(err);
    return DLDB89_OK;
}

static const char *SCHEMA_SQL[] = {
    "CREATE TABLE IF NOT EXISTS dldb_meta (key TEXT PRIMARY KEY,"
    " value BLOB NOT NULL) WITHOUT ROWID;",
    "CREATE TABLE IF NOT EXISTS dldb_relation (id INTEGER PRIMARY KEY,"
    " name BLOB NOT NULL, arity INTEGER NOT NULL,"
    " derived INTEGER NOT NULL DEFAULT 0, UNIQUE(name, arity));",
    "CREATE TABLE IF NOT EXISTS dldb_value (id INTEGER PRIMARY KEY,"
    " kind INTEGER NOT NULL, value BLOB NOT NULL, UNIQUE(kind, value));",
    "CREATE TABLE IF NOT EXISTS dldb_rule (id INTEGER PRIMARY KEY,"
    " head_relation INTEGER NOT NULL, dlx BLOB NOT NULL UNIQUE);",
    "CREATE TABLE IF NOT EXISTS dldb_nullary (relation INTEGER NOT NULL,"
    " which INTEGER NOT NULL, PRIMARY KEY(relation, which)) WITHOUT ROWID;",
    NULL};

dldb89_status dldb89_schema_create(dldb89 *db)
{
    size_t i;
    dldb89_status st;

    for (i = 0; SCHEMA_SQL[i] != NULL; ++i)
    {
        st = dldb89_exec(db, SCHEMA_SQL[i]);
        if (st != DLDB89_OK)
        {
            return st;
        }
    }
    st = dldb89_exec(db, "INSERT OR IGNORE INTO dldb_meta(key,value)"
                         " VALUES('format','1');");
    if (st != DLDB89_OK)
    {
        return st;
    }
    return dldb89_exec(db, "INSERT OR IGNORE INTO dldb_meta(key,value)"
                           " VALUES('generation','0');");
}

static int meta_equals(dldb89 *db, const char *key, const char *want)
{
    sqlite3_stmt *stmt;
    int rc;
    int ok;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "SELECT value FROM dldb_meta WHERE key=?1", -1,
                            &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return 0;
    }
    ok = 0;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *v;
        int n;

        v = sqlite3_column_text(stmt, 0);
        n = sqlite3_column_bytes(stmt, 0);
        if (v != NULL)
        {
            ok = (int)strlen(want) == n &&
                 memcmp(v, want, (size_t)n) == 0;
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}

dldb89_status dldb89_schema_check(dldb89 *db)
{
    if (!meta_equals(db, "format", "1"))
    {
        return dldb89_fail(db, DLDB89_EFORMAT, 0,
                           "unsupported or missing dldb format");
    }
    return DLDB89_OK;
}

/* ---- Values -------------------------------------------------------------- */

static int bind_blob_compat(sqlite3_stmt *stmt, int index, const void *data,
                            size_t size)
{
    if (size == 0)
    {
        return sqlite3_bind_zeroblob(stmt, index, 0);
    }
    return sqlite3_bind_blob(stmt, index, data, (int)size, SQLITE_TRANSIENT);
}

dldb89_status dldb89_value_intern(dldb89 *db, int kind, const void *data,
                                  size_t size, sqlite3_int64 *out_id)
{
    sqlite3_stmt *stmt;
    int rc;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "INSERT OR IGNORE INTO dldb_value(kind,value)"
                            " VALUES(?1,?2)",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "value intern insert");
    }
    sqlite3_bind_int(stmt, 1, kind);
    bind_blob_compat(stmt, 2, data, size);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
    {
        return dldb89_sql_fail(db, rc, "value intern step");
    }
    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "SELECT id FROM dldb_value"
                            " WHERE kind=?1 AND value=?2",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "value intern lookup");
    }
    sqlite3_bind_int(stmt, 1, kind);
    bind_blob_compat(stmt, 2, data, size);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        return dldb89_fail(db, DLDB89_EINTERNAL, rc,
                           "interned value not found");
    }
    *out_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return DLDB89_OK;
}

dldb89_status dldb89_value_fetch(dldb89 *db, sqlite3_int64 id, int *kind,
                                 unsigned char **out_data, size_t *out_size)
{
    sqlite3_stmt *stmt;
    int rc;
    const void *blob;
    int n;
    unsigned char *copy;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "SELECT kind,value FROM dldb_value WHERE id=?1",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "value fetch");
    }
    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        return dldb89_fail(db, DLDB89_ECORRUPT, rc, "value id not found");
    }
    *kind = sqlite3_column_int(stmt, 0);
    blob = sqlite3_column_blob(stmt, 1);
    n = sqlite3_column_bytes(stmt, 1);
    copy = NULL;
    if (n > 0)
    {
        copy = (unsigned char *)malloc((size_t)n);
        if (copy == NULL)
        {
            sqlite3_finalize(stmt);
            return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        }
        memcpy(copy, blob, (size_t)n);
    }
    *out_data = copy;
    *out_size = (size_t)n;
    sqlite3_finalize(stmt);
    return DLDB89_OK;
}

/* ---- Relations ----------------------------------------------------------- */

static dldb89_status create_relation_tables(dldb89 *db, sqlite3_int64 id,
                                            size_t arity)
{
    sqlite3_str *s;
    char *sql;
    size_t i;
    dldb89_status st;

    if (arity == 0)
    {
        return DLDB89_OK;
    }
    s = sqlite3_str_new(db->sql);
    if (s == NULL)
    {
        return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
    }
    for (i = 0; i < 2; ++i)
    {
        size_t c;

        sqlite3_str_appendf(s, "CREATE TABLE dldb_%c%ld (",
                            i == 0 ? 'e' : 'm', (long)id);
        for (c = 0; c < arity; ++c)
        {
            sqlite3_str_appendf(s, "%sc%d INTEGER NOT NULL",
                                c == 0 ? "" : ", ", (int)c);
        }
        sqlite3_str_appendf(s, ", PRIMARY KEY(");
        for (c = 0; c < arity; ++c)
        {
            sqlite3_str_appendf(s, "%sc%d", c == 0 ? "" : ",", (int)c);
        }
        sqlite3_str_appendf(s, ")) WITHOUT ROWID;");
        if (i == 1)
        {
            for (c = 1; c < arity; ++c)
            {
                sqlite3_str_appendf(s,
                                    "CREATE INDEX dldb_m%ld_i%d ON"
                                    " dldb_m%ld(c%d);",
                                    (long)id, (int)c, (long)id, (int)c);
            }
        }
    }
    sql = sqlite3_str_finish(s);
    if (sql == NULL)
    {
        return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
    }
    st = dldb89_exec(db, sql);
    sqlite3_free(sql);
    return st;
}

dldb89_status dldb89_relation_intern(dldb89 *db, const unsigned char *name,
                                     size_t name_size, size_t arity,
                                     sqlite3_int64 *out_id)
{
    sqlite3_stmt *stmt;
    int rc;
    sqlite3_int64 id;
    size_t cap;
    dldb89_status st;

    cap = db->options.max_arity != 0 ? db->options.max_arity
                                     : DLDB89_DEFAULT_MAX_ARITY;
    if (arity > cap)
    {
        return dldb89_fail(db, DLDB89_EPROGRAM, 0,
                           "relation arity exceeds configured maximum");
    }
    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "SELECT id FROM dldb_relation"
                            " WHERE name=?1 AND arity=?2",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "relation lookup");
    }
    bind_blob_compat(stmt, 1, name, name_size);
    sqlite3_bind_int(stmt, 2, (int)arity);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        *out_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return DLDB89_OK;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
    {
        return dldb89_sql_fail(db, rc, "relation lookup step");
    }
    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "INSERT INTO dldb_relation(name,arity,derived)"
                            " VALUES(?1,?2,0)",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "relation insert");
    }
    bind_blob_compat(stmt, 1, name, name_size);
    sqlite3_bind_int(stmt, 2, (int)arity);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
    {
        return dldb89_sql_fail(db, rc, "relation insert step");
    }
    id = sqlite3_last_insert_rowid(db->sql);
    st = create_relation_tables(db, id, arity);
    if (st != DLDB89_OK)
    {
        return st;
    }
    *out_id = id;
    return DLDB89_OK;
}

dldb89_status dldb89_relation_lookup(dldb89 *db, const unsigned char *name,
                                     size_t name_size, size_t arity,
                                     sqlite3_int64 *out_id)
{
    sqlite3_stmt *stmt;
    int rc;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "SELECT id FROM dldb_relation"
                            " WHERE name=?1 AND arity=?2",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "relation lookup");
    }
    bind_blob_compat(stmt, 1, name, name_size);
    sqlite3_bind_int(stmt, 2, (int)arity);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        *out_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return DLDB89_OK;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
    {
        return DLDB89_ENOTFOUND;
    }
    return dldb89_sql_fail(db, rc, "relation lookup step");
}

dldb89_status dldb89_relation_arity(dldb89 *db, sqlite3_int64 id,
                                    size_t *out_arity)
{
    sqlite3_stmt *stmt;
    int rc;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, "SELECT arity FROM dldb_relation"
                                     " WHERE id=?1",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "relation arity");
    }
    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        return dldb89_fail(db, DLDB89_ECORRUPT, rc, "relation id not found");
    }
    *out_arity = (size_t)sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return DLDB89_OK;
}

dldb89_status dldb89_relation_table(dldb89 *db, sqlite3_int64 id, int material,
                                    char *buf, size_t cap)
{
    if (cap == 0)
    {
        return dldb89_fail(db, DLDB89_EINVAL, 0, "table buffer too small");
    }
    sqlite3_snprintf((int)cap, buf, "dldb_%c%ld", material != 0 ? 'm' : 'e',
                     (long)id);
    return DLDB89_OK;
}

/* ---- Lifecycle ----------------------------------------------------------- */

static dldb89 *db_alloc(void)
{
    dldb89 *db;

    db = (dldb89 *)malloc(sizeof(*db));
    if (db == NULL)
    {
        return NULL;
    }
    memset(db, 0, sizeof(*db));
    return db;
}

static void db_free(dldb89 *db)
{
    if (db == NULL)
    {
        return;
    }
    free(db->path);
    free(db);
}

static dldb89_status apply_pragmas(dldb89 *db)
{
    dldb89_status st;

    st = dldb89_exec(db, "PRAGMA foreign_keys=OFF;");
    if (st != DLDB89_OK)
    {
        return st;
    }
    st = dldb89_exec(db, "PRAGMA journal_mode=WAL;");
    if (st != DLDB89_OK)
    {
        return st;
    }
    if (db->options.synchronous_full != 0)
    {
        return dldb89_exec(db, "PRAGMA synchronous=FULL;");
    }
    return dldb89_exec(db, "PRAGMA synchronous=NORMAL;");
}

static dldb89_status read_generation(dldb89 *db)
{
    sqlite3_stmt *stmt;
    int rc;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "SELECT value FROM dldb_meta WHERE key='generation'",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "generation read");
    }
    db->generation = 0;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        const unsigned char *v;
        int n;
        char buf[32];
        int copy;

        v = sqlite3_column_text(stmt, 0);
        n = sqlite3_column_bytes(stmt, 0);
        copy = n;
        if (copy > (int)sizeof(buf) - 1)
        {
            copy = (int)sizeof(buf) - 1;
        }
        if (v != NULL && copy > 0)
        {
            memcpy(buf, v, (size_t)copy);
            buf[copy] = '\0';
            db->generation = (unsigned long)strtoul(buf, NULL, 10);
        }
    }
    sqlite3_finalize(stmt);
    return DLDB89_OK;
}

static dldb89_status after_open(dldb89 *db)
{
    dldb89_status st;

    st = apply_pragmas(db);
    if (st != DLDB89_OK)
    {
        return st;
    }
    st = dldb89_schema_check(db);
    if (st != DLDB89_OK)
    {
        return st;
    }
    st = read_generation(db);
    if (st != DLDB89_OK)
    {
        return st;
    }
    db->query_db.ctx = db;
    db->query_db.ops = dldb89_query_ops();
    return DLDB89_OK;
}

static dldb89_status open_impl(dldb89 **out, const char *path,
                               const dldb89_options *options, int create)
{
    dldb89 *db;
    int rc;
    int flags;
    struct stat sb;
    dldb89_status st;

    if (out == NULL || path == NULL)
    {
        return DLDB89_EINVAL;
    }
    *out = NULL;
    if (create != 0 && strcmp(path, ":memory:") != 0)
    {
        if (stat(path, &sb) == 0)
        {
            return DLDB89_EDATABASE;
        }
    }
    db = db_alloc();
    if (db == NULL)
    {
        return DLDB89_ENOMEM;
    }
    dldb89_options_init(&db->options);
    if (options != NULL)
    {
        db->options = *options;
    }
    db->path = (char *)malloc(strlen(path) + 1);
    if (db->path == NULL)
    {
        db_free(db);
        return DLDB89_ENOMEM;
    }
    strcpy(db->path, path);
    dldb89_clear_error(db);
    flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    rc = sqlite3_open_v2(path, &db->sql, flags, NULL);
    if (rc != SQLITE_OK)
    {
        if (db->sql != NULL)
        {
            dldb89_sql_fail(db, rc, "open");
            sqlite3_close(db->sql);
        }
        db_free(db);
        return DLDB89_EDATABASE;
    }
    if (create != 0)
    {
        st = dldb89_schema_create(db);
        if (st != DLDB89_OK)
        {
            sqlite3_close(db->sql);
            db_free(db);
            return st;
        }
    }
    st = after_open(db);
    if (st != DLDB89_OK)
    {
        sqlite3_close(db->sql);
        db_free(db);
        return st;
    }
    *out = db;
    return DLDB89_OK;
}

dldb89_status dldb89_create(dldb89 **out, const char *path,
                            const dldb89_options *options)
{
    return open_impl(out, path, options, 1);
}

dldb89_status dldb89_open(dldb89 **out, const char *path,
                          const dldb89_options *options)
{
    return open_impl(out, path, options, 0);
}

void dldb89_close(dldb89 *db)
{
    if (db == NULL)
    {
        return;
    }
    if (db->sql != NULL)
    {
        sqlite3_close(db->sql);
    }
    db_free(db);
}

const dldb89_error *dldb89_last_error(const dldb89 *db)
{
    if (db == NULL)
    {
        return NULL;
    }
    return &db->error;
}

unsigned long dldb89_generation(const dldb89 *db)
{
    if (db == NULL)
    {
        return 0;
    }
    return db->generation;
}

const dlq89_db *dldb89_query_db(dldb89 *db)
{
    if (db == NULL)
    {
        return NULL;
    }
    return &db->query_db;
}

/* ---- Public load/exec entry points --------------------------------------- */

dldb89_status dldb89_load_program(dldb89 *db, const dlx89_program *program)
{
    if (db == NULL || program == NULL)
    {
        return DLDB89_EINVAL;
    }
    dldb89_clear_error(db);
    return dldb89_ingest(db, program, 0);
}

dldb89_status dldb89_load_text(dldb89 *db, const void *data, size_t size)
{
    dlx89_program *program;
    dlx89_error error;
    dlx89_status xst;
    dldb89_status st;

    if (db == NULL)
    {
        return DLDB89_EINVAL;
    }
    dldb89_clear_error(db);
    program = NULL;
    xst = dlx89_parse(&program, data, size, NULL, &error);
    if (xst != DLX89_OK)
    {
        return dldb89_fail(db, DLDB89_EPROGRAM, (int)xst,
                           "DLX parse failed");
    }
    st = dldb89_ingest(db, program, 0);
    dlx89_program_destroy(program);
    return st;
}

dldb89_status dldb89_replace_rules(dldb89 *db, const dlx89_program *program)
{
    if (db == NULL || program == NULL)
    {
        return DLDB89_EINVAL;
    }
    dldb89_clear_error(db);
    return dldb89_ingest(db, program, 1);
}
