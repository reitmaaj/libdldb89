/* dldb89_eval.c - datalog89 store adapter and fixed-point materialization. */

#include "dldb89_priv.h"

#include <stdlib.h>
#include <string.h>

/* ---- Materialized-table clearing ----------------------------------------- */

static dldb89_status clear_one_table(dldb89 *db, sqlite3_int64 id, size_t arity)
{
    char name[64];
    char sql[128];
    dldb89_status st;

    if (arity == 0)
    {
        return DLDB89_OK;
    }
    st = dldb89_relation_table(db, id, 1, name, sizeof(name));
    if (st != DLDB89_OK)
    {
        return st;
    }
    sqlite3_snprintf((int)sizeof(sql), sql, "DELETE FROM %s;", name);
    return dldb89_exec(db, sql);
}

static dldb89_status clear_materialized(dldb89 *db)
{
    sqlite3_stmt *stmt;
    sqlite3_int64 id;
    size_t arity;
    int rc;
    dldb89_status st;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, "SELECT id,arity FROM dldb_relation", -1,
                            &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "clear relations");
    }
    for (;;)
    {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE)
        {
            break;
        }
        if (rc != SQLITE_ROW)
        {
            sqlite3_finalize(stmt);
            return dldb89_sql_fail(db, rc, "clear relations step");
        }
        id = sqlite3_column_int64(stmt, 0);
        arity = (size_t)sqlite3_column_int(stmt, 1);
        st = clear_one_table(db, id, arity);
        if (st != DLDB89_OK)
        {
            sqlite3_finalize(stmt);
            return st;
        }
    }
    sqlite3_finalize(stmt);
    return dldb89_exec(db, "DELETE FROM dldb_nullary WHERE which=1;");
}

/* ---- Store operations ---------------------------------------------------- */

static int store_insert(void *ctx, datalog89_rel relation, size_t arity,
                        const datalog89_const *tuple, int *inserted)
{
    dldb89 *db;
    sqlite3_str *s;
    char *sql;
    sqlite3_stmt *stmt;
    int rc;
    size_t i;

    db = (dldb89 *)ctx;
    *inserted = 0;
    s = sqlite3_str_new(db->sql);
    if (s == NULL)
    {
        dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        return 1;
    }
    if (arity == 0)
    {
        sqlite3_str_appendf(s,
                            "INSERT OR IGNORE INTO dldb_nullary(relation,which)"
                            " VALUES(%ld,1)",
                            (long)relation);
    }
    else
    {
        sqlite3_str_appendf(s, "INSERT OR IGNORE INTO dldb_m%ld (",
                            (long)relation);
        for (i = 0; i < arity; ++i)
        {
            sqlite3_str_appendf(s, "%sc%d", i == 0 ? "" : ",", (int)i);
        }
        sqlite3_str_appendf(s, ") VALUES(");
        for (i = 0; i < arity; ++i)
        {
            sqlite3_str_appendf(s, "%s?", i == 0 ? "" : ",");
        }
        sqlite3_str_appendf(s, ")");
    }
    sql = sqlite3_str_finish(s);
    if (sql == NULL)
    {
        dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        return 1;
    }
    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, sql, -1, &stmt, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK)
    {
        dldb89_sql_fail(db, rc, "insert prepare");
        return 1;
    }
    if (arity == 0)
    {
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)relation);
    }
    else
    {
        for (i = 0; i < arity; ++i)
        {
            sqlite3_bind_int64(stmt, (int)(i + 1), (sqlite3_int64)tuple[i]);
        }
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        sqlite3_finalize(stmt);
        dldb89_sql_fail(db, rc, "insert step");
        return 1;
    }
    *inserted = sqlite3_changes(db->sql) > 0;
    sqlite3_finalize(stmt);
    return 0;
}

struct dldb89_store_scan
{
    sqlite3_stmt *stmt;
    size_t arity;
    int nullary_done;
};

static int store_scan_open(void *ctx, datalog89_rel relation, size_t arity,
                           const datalog89_const *values,
                           const unsigned char *bound,
                           datalog89_scan **out)
{
    dldb89 *db;
    struct dldb89_store_scan *scan;
    sqlite3_str *s;
    char *sql;
    int first;
    int rc;
    size_t i;

    db = (dldb89 *)ctx;
    *out = NULL;
    scan = (struct dldb89_store_scan *)malloc(sizeof(*scan));
    if (scan == NULL)
    {
        dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        return 1;
    }
    scan->stmt = NULL;
    scan->arity = arity;
    scan->nullary_done = 0;
    s = sqlite3_str_new(db->sql);
    if (s == NULL)
    {
        free(scan);
        dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        return 1;
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
            sqlite3_str_appendf(s, "%sc%d", i == 0 ? "" : ",", (int)i);
        }
        sqlite3_str_appendf(s, " FROM dldb_m%ld", (long)relation);
        first = 1;
        for (i = 0; i < arity; ++i)
        {
            if (bound[i] != 0)
            {
                sqlite3_str_appendf(s, "%sc%d=?",
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
        return 1;
    }
    rc = sqlite3_prepare_v2(db->sql, sql, -1, &scan->stmt, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK)
    {
        free(scan);
        dldb89_sql_fail(db, rc, "scan prepare");
        return 1;
    }
    if (arity > 0)
    {
        int bind_index;

        bind_index = 1;
        for (i = 0; i < arity; ++i)
        {
            if (bound[i] != 0)
            {
                sqlite3_bind_int64(scan->stmt, bind_index,
                                   (sqlite3_int64)values[i]);
                bind_index += 1;
            }
        }
    }
    *out = (datalog89_scan *)scan;
    return 0;
}

static int store_scan_next(void *ctx, datalog89_scan *handle,
                           datalog89_const *tuple, int *found)
{
    dldb89 *db;
    struct dldb89_store_scan *scan;
    int rc;
    size_t i;

    db = (dldb89 *)ctx;
    scan = (struct dldb89_store_scan *)handle;
    *found = 0;
    if (scan->arity == 0)
    {
        if (scan->nullary_done != 0)
        {
            return 0;
        }
        scan->nullary_done = 1;
        *found = 1;
        return 0;
    }
    rc = sqlite3_step(scan->stmt);
    if (rc == SQLITE_ROW)
    {
        for (i = 0; i < scan->arity; ++i)
        {
            tuple[i] = (datalog89_const)sqlite3_column_int64(scan->stmt,
                                                             (int)i);
        }
        *found = 1;
        return 0;
    }
    if (rc == SQLITE_DONE)
    {
        return 0;
    }
    dldb89_sql_fail(db, rc, "scan step");
    return 1;
}

static void store_scan_close(void *ctx, datalog89_scan *handle)
{
    struct dldb89_store_scan *scan;

    (void)ctx;
    scan = (struct dldb89_store_scan *)handle;
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

static const datalog89_store_ops STORE_OPS = {
    store_insert, store_scan_open, store_scan_next, store_scan_close};

/* ---- Seeding and rule loading -------------------------------------------- */

static dldb89_status seed_nullary(dldb89 *db, datalog89_eval *eval,
                                  sqlite3_int64 id)
{
    sqlite3_stmt *stmt;
    int rc;
    datalog89_status st;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "SELECT 1 FROM dldb_nullary"
                            " WHERE relation=?1 AND which=0",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "nullary edb");
    }
    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW)
    {
        st = datalog89_eval_add_fact(eval, (datalog89_rel)id, 0, NULL);
        if (st != DATALOG89_OK)
        {
            return dldb89_fail(db, DLDB89_EDATABASE, (int)st, "seed fact");
        }
    }
    return DLDB89_OK;
}

static dldb89_status seed_relation(dldb89 *db, datalog89_eval *eval,
                                   sqlite3_int64 id, size_t arity)
{
    sqlite3_stmt *stmt;
    sqlite3_str *s;
    char *sql;
    datalog89_const *row;
    size_t i;
    int rc;
    dldb89_status st;

    if (arity == 0)
    {
        return seed_nullary(db, eval, id);
    }
    row = (datalog89_const *)malloc(arity * sizeof(*row));
    if (row == NULL)
    {
        return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
    }
    s = sqlite3_str_new(db->sql);
    if (s == NULL)
    {
        free(row);
        return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
    }
    sqlite3_str_appendf(s, "SELECT ");
    for (i = 0; i < arity; ++i)
    {
        sqlite3_str_appendf(s, "%sc%d", i == 0 ? "" : ",", (int)i);
    }
    sqlite3_str_appendf(s, " FROM dldb_e%ld", (long)id);
    sql = sqlite3_str_finish(s);
    if (sql == NULL)
    {
        free(row);
        return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
    }
    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, sql, -1, &stmt, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK)
    {
        free(row);
        return dldb89_sql_fail(db, rc, "seed prepare");
    }
    st = DLDB89_OK;
    for (;;)
    {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE)
        {
            break;
        }
        if (rc != SQLITE_ROW)
        {
            st = dldb89_sql_fail(db, rc, "seed step");
            break;
        }
        for (i = 0; i < arity; ++i)
        {
            row[i] = (datalog89_const)sqlite3_column_int64(stmt, (int)i);
        }
        if (datalog89_eval_add_fact(eval, (datalog89_rel)id, arity, row) !=
            DATALOG89_OK)
        {
            st = dldb89_fail(db, DLDB89_EDATABASE, 0, "seed fact rejected");
            break;
        }
    }
    sqlite3_finalize(stmt);
    free(row);
    return st;
}

static dldb89_status seed_edb(dldb89 *db, datalog89_eval *eval)
{
    sqlite3_stmt *stmt;
    sqlite3_int64 id;
    size_t arity;
    int rc;
    dldb89_status st;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, "SELECT id,arity FROM dldb_relation", -1,
                            &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "seed relations");
    }
    st = DLDB89_OK;
    for (;;)
    {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE)
        {
            break;
        }
        if (rc != SQLITE_ROW)
        {
            st = dldb89_sql_fail(db, rc, "seed relations step");
            break;
        }
        id = sqlite3_column_int64(stmt, 0);
        arity = (size_t)sqlite3_column_int(stmt, 1);
        st = seed_relation(db, eval, id, arity);
        if (st != DLDB89_OK)
        {
            break;
        }
    }
    sqlite3_finalize(stmt);
    return st;
}

/* ---- Rule translation ---------------------------------------------------- */

struct vmap
{
    const unsigned char *name;
    size_t len;
    datalog89_var var;
};

static datalog89_var vmap_get(struct vmap *map, size_t *count,
                              const unsigned char *name, size_t len)
{
    size_t i;

    for (i = 0; i < *count; ++i)
    {
        if (map[i].len == len &&
            (len == 0 || memcmp(map[i].name, name, len) == 0))
        {
            return map[i].var;
        }
    }
    map[*count].name = name;
    map[*count].len = len;
    map[*count].var = (datalog89_var)(*count);
    *count = *count + 1;
    return (datalog89_var)(*count - 1);
}

static dldb89_status intern_dlx_term(dldb89 *db, const dlx89_term *term,
                                     datalog89_term *out)
{
    unsigned char *buf;
    size_t n;
    int kind;
    sqlite3_int64 id;
    dldb89_status st;

    if (term->kind == DLX89_TERM_STRING)
    {
        out->kind = DATALOG89_TERM_CONST;
        st = dldb89_value_intern(db, 1, term->name.data, term->name.len, &id);
        out->u.constant = (datalog89_const)id;
        return st;
    }
    n = term->integer.len + (term->integer.negative != 0 ? 1 : 0);
    buf = (unsigned char *)malloc(n == 0 ? 1 : n);
    if (buf == NULL)
    {
        return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
    }
    kind = 2;
    if (term->integer.negative != 0)
    {
        buf[0] = '-';
        memcpy(buf + 1, term->integer.digits, term->integer.len);
    }
    else if (term->integer.len != 0)
    {
        memcpy(buf, term->integer.digits, term->integer.len);
    }
    st = dldb89_value_intern(db, kind, buf, n, &id);
    free(buf);
    out->kind = DATALOG89_TERM_CONST;
    out->u.constant = (datalog89_const)id;
    return st;
}

static dldb89_status translate_atom(dldb89 *db, const dlx89_atom *atom,
                                    datalog89_atom *out, struct vmap *map,
                                    size_t *map_count)
{
    datalog89_term *terms;
    sqlite3_int64 id;
    size_t i;
    dldb89_status st;

    st = dldb89_relation_lookup(db, atom->predicate.data, atom->predicate.len,
                                atom->arity, &id);
    if (st != DLDB89_OK)
    {
        return dldb89_fail(db, DLDB89_ECORRUPT, 0,
                           "rule relation missing from catalog");
    }
    out->relation = (datalog89_rel)id;
    out->arity = atom->arity;
    out->terms = NULL;
    if (atom->arity == 0)
    {
        return DLDB89_OK;
    }
    terms = (datalog89_term *)malloc(atom->arity * sizeof(*terms));
    if (terms == NULL)
    {
        return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
    }
    for (i = 0; i < atom->arity; ++i)
    {
        if (atom->terms[i].kind == DLX89_TERM_VARIABLE)
        {
            terms[i].kind = DATALOG89_TERM_VAR;
            terms[i].u.variable =
                vmap_get(map, map_count, atom->terms[i].name.data,
                         atom->terms[i].name.len);
        }
        else
        {
            st = intern_dlx_term(db, &atom->terms[i], &terms[i]);
            if (st != DLDB89_OK)
            {
                free(terms);
                return st;
            }
        }
    }
    out->terms = terms;
    return DLDB89_OK;
}

static void free_atom_terms(datalog89_atom *atom)
{
    free((void *)atom->terms);
    atom->terms = NULL;
}

static dldb89_status translate_rule(dldb89 *db, datalog89_eval *eval,
                                    const dlx89_clause *clause)
{
    datalog89_rule rule;
    datalog89_atom *body;
    struct vmap *map;
    size_t map_count;
    size_t total_terms;
    size_t i;
    datalog89_status dst;
    dldb89_status st;

    total_terms = clause->head.arity;
    for (i = 0; i < clause->body_count; ++i)
    {
        total_terms += clause->body[i].arity;
    }
    map = (struct vmap *)malloc((total_terms == 0 ? 1 : total_terms) *
                                sizeof(*map));
    if (map == NULL)
    {
        return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
    }
    map_count = 0;
    memset(&rule, 0, sizeof(rule));
    body = NULL;
    st = translate_atom(db, &clause->head, &rule.head, map, &map_count);
    if (st != DLDB89_OK)
    {
        free(map);
        return st;
    }
    if (clause->body_count != 0)
    {
        body = (datalog89_atom *)malloc(clause->body_count * sizeof(*body));
        if (body == NULL)
        {
            free_atom_terms(&rule.head);
            free(map);
            return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        }
        memset(body, 0, clause->body_count * sizeof(*body));
    }
    rule.body_count = clause->body_count;
    rule.body = body;
    st = DLDB89_OK;
    for (i = 0; i < clause->body_count; ++i)
    {
        st = translate_atom(db, &clause->body[i], &body[i], map, &map_count);
        if (st != DLDB89_OK)
        {
            break;
        }
    }
    if (st == DLDB89_OK)
    {
        dst = datalog89_eval_add_rule(eval, &rule);
        if (dst != DATALOG89_OK)
        {
            st = dldb89_fail(db, DLDB89_EPROGRAM, (int)dst,
                             "evaluator rejected rule");
        }
    }
    free_atom_terms(&rule.head);
    for (i = 0; i < clause->body_count; ++i)
    {
        free_atom_terms(&body[i]);
    }
    free(body);
    free(map);
    return st;
}

static dldb89_status load_rules(dldb89 *db, datalog89_eval *eval)
{
    sqlite3_stmt *stmt;
    unsigned char *buf;
    size_t len;
    size_t cap;
    int have;
    int rc;
    dlx89_program *program;
    dlx89_error error;
    dlx89_status pst;
    dldb89_status st;
    size_t i;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, "SELECT dlx FROM dldb_rule ORDER BY id",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "load rules");
    }
    buf = NULL;
    len = 0;
    cap = 0;
    have = 0;
    st = DLDB89_OK;
    for (;;)
    {
        const void *blob;
        int n;

        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE)
        {
            break;
        }
        if (rc != SQLITE_ROW)
        {
            st = dldb89_sql_fail(db, rc, "load rules step");
            break;
        }
        blob = sqlite3_column_blob(stmt, 0);
        n = sqlite3_column_bytes(stmt, 0);
        if (len + (size_t)n + 1 > cap)
        {
            size_t ncap;
            unsigned char *grown;

            ncap = cap == 0 ? 256 : cap * 2;
            while (ncap < len + (size_t)n + 1)
            {
                ncap = ncap * 2;
            }
            grown = (unsigned char *)realloc(buf, ncap);
            if (grown == NULL)
            {
                st = dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
                break;
            }
            buf = grown;
            cap = ncap;
        }
        if (n > 0)
        {
            memcpy(buf + len, blob, (size_t)n);
        }
        len += (size_t)n;
        have = 1;
    }
    sqlite3_finalize(stmt);
    if (st != DLDB89_OK || have == 0)
    {
        free(buf);
        return st;
    }
    buf[len] = '\0';
    program = NULL;
    pst = dlx89_parse(&program, buf, len, NULL, &error);
    free(buf);
    if (pst != DLX89_OK)
    {
        return dldb89_fail(db, DLDB89_ECORRUPT, (int)pst,
                           "stored rule is not valid DLX");
    }
    st = DLDB89_OK;
    for (i = 0; i < dlx89_program_clause_count(program); ++i)
    {
        st = translate_rule(db, eval, dlx89_program_clause_at(program, i));
        if (st != DLDB89_OK)
        {
            break;
        }
    }
    dlx89_program_destroy(program);
    return st;
}

/* ---- Rebuild ------------------------------------------------------------- */

dldb89_status dldb89_rebuild(dldb89 *db)
{
    datalog89_eval_config config;
    datalog89_eval *eval;
    datalog89_status dst;
    dldb89_status st;

    st = clear_materialized(db);
    if (st != DLDB89_OK)
    {
        return st;
    }
    memset(&config, 0, sizeof(config));
    config.store.ctx = db;
    config.store.ops = &STORE_OPS;
    eval = NULL;
    dst = datalog89_eval_create(&config, &eval);
    if (dst != DATALOG89_OK)
    {
        return dldb89_fail(db, DLDB89_EINTERNAL, (int)dst,
                           "evaluator create failed");
    }
    st = seed_edb(db, eval);
    if (st == DLDB89_OK)
    {
        st = load_rules(db, eval);
    }
    if (st == DLDB89_OK)
    {
        dst = datalog89_eval_run(eval);
        if (dst != DATALOG89_OK)
        {
            st = dldb89_fail(db, DLDB89_EDATABASE, (int)dst,
                             "evaluation failed");
        }
    }
    datalog89_eval_destroy(eval);
    return st;
}
