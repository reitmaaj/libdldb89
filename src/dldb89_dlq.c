/* dldb89_dlq.c - libdlq89 database adapter over SQLite-backed relations. */

#include "dldb89_priv.h"

#include <stdlib.h>
#include <string.h>

struct dldb89_snapshot
{
    dldb89 *db;
};

struct dldb89_qscan
{
    sqlite3_stmt *stmt;
    size_t arity;
    int nullary_done;
};

static int q_snapshot_open(void *ctx, dlq89_snapshot **out)
{
    dldb89 *db;
    struct dldb89_snapshot *snap;

    db = (dldb89 *)ctx;
    *out = NULL;
    if (db->in_write != 0)
    {
        dldb89_fail(db, DLDB89_EBUSY, 0, "write in progress");
        return DLQ89_DB_ERROR;
    }
    snap = (struct dldb89_snapshot *)malloc(sizeof(*snap));
    if (snap == NULL)
    {
        dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        return DLQ89_DB_ERROR;
    }
    snap->db = db;
    if (dldb89_exec(db, "BEGIN DEFERRED;") != DLDB89_OK)
    {
        free(snap);
        return DLQ89_DB_ERROR;
    }
    *out = (dlq89_snapshot *)snap;
    return DLQ89_DB_OK;
}

static void q_snapshot_close(void *ctx, dlq89_snapshot *handle)
{
    dldb89 *db;

    db = (dldb89 *)ctx;
    dldb89_exec(db, "COMMIT;");
    free(handle);
}

static int q_relation(void *ctx, dlq89_snapshot *snapshot,
                      const unsigned char *name, size_t name_size,
                      size_t arity, dlq89_relation *out)
{
    dldb89 *db;
    sqlite3_stmt *stmt;
    int rc;

    (void)snapshot;
    db = (dldb89 *)ctx;
    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "SELECT id FROM dldb_relation"
                            " WHERE name=?1 AND arity=?2",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        dldb89_sql_fail(db, rc, "dlq relation");
        return DLQ89_DB_ERROR;
    }
    if (name_size == 0)
    {
        sqlite3_bind_zeroblob(stmt, 1, 0);
    }
    else
    {
        sqlite3_bind_blob(stmt, 1, name, (int)name_size, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, 2, (int)arity);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        *out = (dlq89_relation)sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return DLQ89_DB_OK;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
    {
        return DLQ89_DB_NOT_FOUND;
    }
    dldb89_sql_fail(db, rc, "dlq relation step");
    return DLQ89_DB_ERROR;
}

static int q_scan_open(void *ctx, dlq89_snapshot *snapshot,
                       dlq89_relation relation, size_t arity,
                       const dlq89_value *values, const unsigned char *bound,
                       dlq89_scan **out)
{
    dldb89 *db;
    struct dldb89_qscan *scan;
    sqlite3_str *s;
    char *sql;
    size_t i;
    int first;
    int rc;

    (void)snapshot;
    db = (dldb89 *)ctx;
    *out = NULL;
    scan = (struct dldb89_qscan *)malloc(sizeof(*scan));
    if (scan == NULL)
    {
        dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        return DLQ89_DB_ERROR;
    }
    scan->stmt = NULL;
    scan->arity = arity;
    scan->nullary_done = 0;
    s = sqlite3_str_new(db->sql);
    if (s == NULL)
    {
        free(scan);
        dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        return DLQ89_DB_ERROR;
    }
    if (arity == 0)
    {
        sqlite3_str_appendf(s,
                            "SELECT 1 FROM dldb_nullary"
                            " WHERE relation=%ld AND which=1",
                            (long)relation);
    }
    else
    {
        sqlite3_str_appendf(s, "SELECT ");
        for (i = 0; i < arity; ++i)
        {
            sqlite3_str_appendf(s, "%sv%d.kind,v%d.value",
                                i == 0 ? "" : ",", (int)i, (int)i);
        }
        sqlite3_str_appendf(s, " FROM dldb_m%ld AS t", (long)relation);
        for (i = 0; i < arity; ++i)
        {
            sqlite3_str_appendf(s, " JOIN dldb_value AS v%d ON v%d.id=t.c%d",
                                (int)i, (int)i, (int)i);
        }
        first = 1;
        for (i = 0; i < arity; ++i)
        {
            if (bound[i] != 0)
            {
                sqlite3_str_appendf(s,
                                    "%st.c%d=(SELECT id FROM dldb_value"
                                    " WHERE kind=? AND value=?)",
                                    first != 0 ? " WHERE " : " AND ", (int)i);
                first = 0;
            }
        }
    }
    sql = sqlite3_str_finish(s);
    if (sql == NULL)
    {
        free(scan);
        dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        return DLQ89_DB_ERROR;
    }
    rc = sqlite3_prepare_v2(db->sql, sql, -1, &scan->stmt, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK)
    {
        free(scan);
        dldb89_sql_fail(db, rc, "dlq scan prepare");
        return DLQ89_DB_ERROR;
    }
    if (arity > 0)
    {
        int index;

        index = 1;
        for (i = 0; i < arity; ++i)
        {
            if (bound[i] != 0)
            {
                sqlite3_bind_int(scan->stmt, index, (int)values[i].kind);
                index += 1;
                if (values[i].size == 0)
                {
                    sqlite3_bind_zeroblob(scan->stmt, index, 0);
                }
                else
                {
                    sqlite3_bind_blob(scan->stmt, index, values[i].data,
                                      (int)values[i].size, SQLITE_TRANSIENT);
                }
                index += 1;
            }
        }
    }
    *out = (dlq89_scan *)scan;
    return DLQ89_DB_OK;
}

static int q_scan_next(void *ctx, dlq89_scan *handle, dlq89_value *tuple,
                       int *found)
{
    dldb89 *db;
    struct dldb89_qscan *scan;
    int rc;
    size_t i;

    db = (dldb89 *)ctx;
    scan = (struct dldb89_qscan *)handle;
    *found = 0;
    if (scan->arity == 0)
    {
        if (scan->nullary_done != 0)
        {
            return DLQ89_DB_OK;
        }
        scan->nullary_done = 1;
        *found = 1;
        return DLQ89_DB_OK;
    }
    rc = sqlite3_step(scan->stmt);
    if (rc == SQLITE_ROW)
    {
        for (i = 0; i < scan->arity; ++i)
        {
            tuple[i].kind =
                (dlq89_value_kind)sqlite3_column_int(scan->stmt, (int)(2 * i));
            tuple[i].data = (const unsigned char *)sqlite3_column_blob(
                scan->stmt, (int)(2 * i + 1));
            tuple[i].size = (size_t)sqlite3_column_bytes(scan->stmt,
                                                         (int)(2 * i + 1));
        }
        *found = 1;
        return DLQ89_DB_OK;
    }
    if (rc == SQLITE_DONE)
    {
        return DLQ89_DB_OK;
    }
    dldb89_sql_fail(db, rc, "dlq scan step");
    return DLQ89_DB_ERROR;
}

static void q_scan_close(void *ctx, dlq89_scan *handle)
{
    struct dldb89_qscan *scan;

    (void)ctx;
    scan = (struct dldb89_qscan *)handle;
    if (scan == NULL)
    {
        return;
    }
    if (scan->stmt != NULL)
    {
        sqlite3_finalize(scan->stmt);
    }
    free(scan);
}

static const dlq89_db_ops QUERY_OPS = {q_snapshot_open, q_snapshot_close,
                                       q_relation,      q_scan_open,
                                       q_scan_next,     q_scan_close};

const dlq89_db_ops *dldb89_query_ops(void)
{
    return &QUERY_OPS;
}
