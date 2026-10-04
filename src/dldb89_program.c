/* dldb89_program.c - DLX ingestion, canonical dump, and integrity checks. */

#include "dldb89_priv.h"

#include <stdlib.h>
#include <string.h>

/* ---- Growable byte buffer ------------------------------------------------ */

struct rbuf
{
    unsigned char *data;
    size_t len;
    size_t cap;
    int failed;
};

static void rbuf_init(struct rbuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->failed = 0;
}

static int rbuf_reserve(struct rbuf *b, size_t extra)
{
    size_t need;
    size_t ncap;
    unsigned char *grown;

    need = b->len + extra;
    if (need <= b->cap)
    {
        return 0;
    }
    ncap = b->cap == 0 ? 256 : b->cap;
    while (ncap < need)
    {
        ncap = ncap * 2;
    }
    grown = (unsigned char *)realloc(b->data, ncap);
    if (grown == NULL)
    {
        b->failed = 1;
        return 1;
    }
    b->data = grown;
    b->cap = ncap;
    return 0;
}

static int rbuf_write(void *ctx, const unsigned char *bytes, size_t len)
{
    struct rbuf *b;

    b = (struct rbuf *)ctx;
    if (rbuf_reserve(b, len) != 0)
    {
        return 1;
    }
    if (len != 0)
    {
        memcpy(b->data + b->len, bytes, len);
    }
    b->len += len;
    return 0;
}

static void rbuf_free(struct rbuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

struct clause_list
{
    struct rbuf *items;
    size_t count;
    size_t cap;
};

static int clause_add(struct clause_list *list, const unsigned char *bytes,
                      size_t len)
{
    struct rbuf *grown;

    if (list->count == list->cap)
    {
        size_t ncap;

        ncap = list->cap == 0 ? 16 : list->cap * 2;
        grown = (struct rbuf *)realloc(list->items, ncap * sizeof(*grown));
        if (grown == NULL)
        {
            return 1;
        }
        list->items = grown;
        list->cap = ncap;
    }
    rbuf_init(&list->items[list->count]);
    if (rbuf_reserve(&list->items[list->count], len) != 0)
    {
        return 1;
    }
    if (len != 0)
    {
        memcpy(list->items[list->count].data, bytes, len);
    }
    list->items[list->count].len = len;
    list->count = list->count + 1;
    return 0;
}

static void clause_list_free(struct clause_list *list)
{
    size_t i;

    for (i = 0; i < list->count; ++i)
    {
        rbuf_free(&list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

/* ---- Ingestion ----------------------------------------------------------- */

static dldb89_status intern_atom_relations(dldb89 *db, const dlx89_atom *atom)
{
    sqlite3_int64 id;

    return dldb89_relation_intern(db, atom->predicate.data,
                                  atom->predicate.len, atom->arity, &id);
}

static dldb89_status intern_rule_relations(dldb89 *db,
                                           const dlx89_clause *clause)
{
    size_t i;
    dldb89_status st;

    st = intern_atom_relations(db, &clause->head);
    if (st != DLDB89_OK)
    {
        return st;
    }
    for (i = 0; i < clause->body_count; ++i)
    {
        st = intern_atom_relations(db, &clause->body[i]);
        if (st != DLDB89_OK)
        {
            return st;
        }
    }
    return DLDB89_OK;
}

static dldb89_status insert_rule(dldb89 *db, const dlx89_clause *clause)
{
    struct rbuf buf;
    dlx89_sink sink;
    dlx89_error error;
    dlx89_status xst;
    sqlite3_stmt *stmt;
    sqlite3_int64 head_id;
    int rc;
    dldb89_status st;

    st = dldb89_relation_intern(db, clause->head.predicate.data,
                                clause->head.predicate.len,
                                clause->head.arity, &head_id);
    if (st != DLDB89_OK)
    {
        return st;
    }
    rbuf_init(&buf);
    sink.write = rbuf_write;
    sink.ctx = &buf;
    xst = dlx89_write_clause(clause, &sink, &error);
    if (xst != DLX89_OK)
    {
        rbuf_free(&buf);
        return dldb89_fail(db, DLDB89_EPROGRAM, (int)xst,
                           "canonical rule write failed");
    }
    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "INSERT OR IGNORE INTO dldb_rule(head_relation,dlx)"
                            " VALUES(?1,?2)",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        rbuf_free(&buf);
        return dldb89_sql_fail(db, rc, "rule insert");
    }
    sqlite3_bind_int64(stmt, 1, head_id);
    sqlite3_bind_blob(stmt, 2, buf.data, (int)buf.len, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    rbuf_free(&buf);
    if (rc != SQLITE_DONE)
    {
        return dldb89_sql_fail(db, rc, "rule insert step");
    }
    return DLDB89_OK;
}

static dldb89_status insert_fact(dldb89 *db, const dlx89_clause *clause,
                                 sqlite3_int64 rel_id, size_t arity)
{
    sqlite3_int64 *ids;
    sqlite3_str *s;
    char *sql;
    sqlite3_stmt *stmt;
    size_t i;
    int rc;
    dldb89_status st;

    ids = NULL;
    if (arity > 0)
    {
        ids = (sqlite3_int64 *)malloc(arity * sizeof(*ids));
        if (ids == NULL)
        {
            return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        }
    }
    st = DLDB89_OK;
    for (i = 0; i < arity; ++i)
    {
        const dlx89_term *term;

        term = &clause->head.terms[i];
        if (term->kind == DLX89_TERM_STRING)
        {
            st = dldb89_value_intern(db, 1, term->name.data, term->name.len,
                                     &ids[i]);
        }
        else if (term->kind == DLX89_TERM_INTEGER)
        {
            size_t n;
            unsigned char *tmp;

            n = term->integer.len + (term->integer.negative != 0 ? 1 : 0);
            tmp = (unsigned char *)malloc(n == 0 ? 1 : n);
            if (tmp == NULL)
            {
                st = dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
            }
            else
            {
                if (term->integer.negative != 0)
                {
                    tmp[0] = '-';
                    memcpy(tmp + 1, term->integer.digits,
                           term->integer.len);
                }
                else if (term->integer.len != 0)
                {
                    memcpy(tmp, term->integer.digits, term->integer.len);
                }
                st = dldb89_value_intern(db, 2, tmp, n, &ids[i]);
                free(tmp);
            }
        }
        else
        {
            st = dldb89_fail(db, DLDB89_EPROGRAM, 0,
                             "fact term is not a constant");
        }
        if (st != DLDB89_OK)
        {
            free(ids);
            return st;
        }
    }
    if (arity == 0)
    {
        rc = sqlite3_prepare_v2(db->sql,
                                "INSERT OR IGNORE INTO dldb_nullary"
                                "(relation,which) VALUES(?1,0)",
                                -1, &stmt, NULL);
        if (rc == SQLITE_OK)
        {
            sqlite3_bind_int64(stmt, 1, rel_id);
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    else
    {
        s = sqlite3_str_new(db->sql);
        if (s == NULL)
        {
            free(ids);
            return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        }
        sqlite3_str_appendf(s, "INSERT OR IGNORE INTO dldb_e%ld (",
                            (long)rel_id);
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
        sql = sqlite3_str_finish(s);
        if (sql == NULL)
        {
            free(ids);
            return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        }
        stmt = NULL;
        rc = sqlite3_prepare_v2(db->sql, sql, -1, &stmt, NULL);
        sqlite3_free(sql);
        if (rc == SQLITE_OK)
        {
            for (i = 0; i < arity; ++i)
            {
                sqlite3_bind_int64(stmt, (int)(i + 1), ids[i]);
            }
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    free(ids);
    if (rc != SQLITE_DONE)
    {
        return dldb89_sql_fail(db, rc, "fact insert");
    }
    return DLDB89_OK;
}

static dldb89_status ingest_clause(dldb89 *db, const dlx89_clause *clause,
                                   int mode)
{
    sqlite3_int64 rel_id;
    dldb89_status st;

    if (clause->is_rule != 0)
    {
        st = intern_rule_relations(db, clause);
        if (st != DLDB89_OK)
        {
            return st;
        }
        return insert_rule(db, clause);
    }
    if (mode == 1)
    {
        return dldb89_fail(db, DLDB89_EPROGRAM, 0,
                           "fact clause not allowed when replacing rules");
    }
    st = dldb89_relation_intern(db, clause->head.predicate.data,
                                clause->head.predicate.len,
                                clause->head.arity, &rel_id);
    if (st != DLDB89_OK)
    {
        return st;
    }
    return insert_fact(db, clause, rel_id, clause->head.arity);
}

static dldb89_status recompute_derived(dldb89 *db)
{
    dldb89_status st;

    st = dldb89_exec(db, "UPDATE dldb_relation SET derived=0;");
    if (st != DLDB89_OK)
    {
        return st;
    }
    return dldb89_exec(db,
                       "UPDATE dldb_relation SET derived=1 WHERE id IN"
                       " (SELECT head_relation FROM dldb_rule);");
}

static dldb89_status bump_generation(dldb89 *db)
{
    char buf[32];
    sqlite3_stmt *stmt;
    int rc;

    db->generation = db->generation + 1;
    sqlite3_snprintf((int)sizeof(buf), buf, "%lu", db->generation);
    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql,
                            "UPDATE dldb_meta SET value=?1"
                            " WHERE key='generation'",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "generation update");
    }
    sqlite3_bind_text(stmt, 1, buf, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
    {
        return dldb89_sql_fail(db, rc, "generation update step");
    }
    return DLDB89_OK;
}

dldb89_status dldb89_ingest(dldb89 *db, const dlx89_program *program, int mode)
{
    size_t i;
    dldb89_status st;

    if (db->in_write != 0)
    {
        return dldb89_fail(db, DLDB89_EBUSY, 0, "write already in progress");
    }
    st = dldb89_exec(db, "BEGIN IMMEDIATE;");
    if (st != DLDB89_OK)
    {
        return st;
    }
    db->in_write = 1;
    if (mode == 1)
    {
        st = dldb89_exec(db, "DELETE FROM dldb_rule;");
    }
    for (i = 0; i < dlx89_program_clause_count(program) && st == DLDB89_OK;
         ++i)
    {
        st = ingest_clause(db, dlx89_program_clause_at(program, i), mode);
    }
    if (st == DLDB89_OK)
    {
        st = recompute_derived(db);
    }
    if (st == DLDB89_OK)
    {
        st = dldb89_rebuild(db);
    }
    if (st == DLDB89_OK)
    {
        st = bump_generation(db);
    }
    if (st == DLDB89_OK)
    {
        st = dldb89_exec(db, "COMMIT;");
    }
    else
    {
        dldb89_exec(db, "ROLLBACK;");
    }
    db->in_write = 0;
    return st;
}

/* ---- Canonical dump ------------------------------------------------------ */

static int clause_cmp(const void *a, const void *b)
{
    const struct rbuf *x;
    const struct rbuf *y;
    size_t n;
    int c;

    x = (const struct rbuf *)a;
    y = (const struct rbuf *)b;
    n = x->len < y->len ? x->len : y->len;
    c = 0;
    if (n != 0)
    {
        c = memcmp(x->data, y->data, n);
    }
    if (c != 0)
    {
        return c;
    }
    if (x->len < y->len)
    {
        return -1;
    }
    if (x->len > y->len)
    {
        return 1;
    }
    return 0;
}

static dldb89_status fetch_relation_name(dldb89 *db, sqlite3_int64 id,
                                         unsigned char **out, size_t *out_len)
{
    sqlite3_stmt *stmt;
    int rc;
    const void *blob;
    int n;
    unsigned char *copy;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, "SELECT name FROM dldb_relation WHERE id=?1",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "relation name");
    }
    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        return dldb89_fail(db, DLDB89_ECORRUPT, rc, "relation missing");
    }
    blob = sqlite3_column_blob(stmt, 0);
    n = sqlite3_column_bytes(stmt, 0);
    copy = (unsigned char *)malloc(n == 0 ? 1 : (size_t)n);
    if (copy == NULL)
    {
        sqlite3_finalize(stmt);
        return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
    }
    if (n > 0)
    {
        memcpy(copy, blob, (size_t)n);
    }
    *out = copy;
    *out_len = (size_t)n;
    sqlite3_finalize(stmt);
    return DLDB89_OK;
}

static dldb89_status render_fact_row(dldb89 *db,
                                     const unsigned char *name,
                                     size_t name_len, size_t arity,
                                     sqlite3_stmt *row,
                                     struct clause_list *list)
{
    dlx89_clause clause;
    dlx89_atom atom;
    dlx89_term *terms;
    unsigned char **payloads;
    size_t i;
    struct rbuf out;
    dlx89_sink sink;
    dlx89_error error;
    dlx89_status xst;
    dldb89_status st;

    terms = NULL;
    payloads = NULL;
    if (arity > 0)
    {
        terms = (dlx89_term *)malloc(arity * sizeof(*terms));
        payloads = (unsigned char **)malloc(arity * sizeof(*payloads));
        if (terms == NULL || payloads == NULL)
        {
            free(terms);
            free(payloads);
            return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        }
    }
    st = DLDB89_OK;
    for (i = 0; i < arity; ++i)
    {
        int kind;
        unsigned char *data;
        size_t size;
        sqlite3_int64 value_id;

        value_id = sqlite3_column_int64(row, (int)i);
        st = dldb89_value_fetch(db, value_id, &kind, &data, &size);
        if (st != DLDB89_OK)
        {
            break;
        }
        payloads[i] = data;
        memset(&terms[i], 0, sizeof(terms[i]));
        if (kind == 1)
        {
            terms[i].kind = DLX89_TERM_STRING;
            terms[i].name.data = data;
            terms[i].name.len = size;
        }
        else
        {
            terms[i].kind = DLX89_TERM_INTEGER;
            if (size > 0 && data[0] == '-')
            {
                terms[i].integer.negative = 1;
                terms[i].integer.digits = data + 1;
                terms[i].integer.len = size - 1;
            }
            else
            {
                terms[i].integer.negative = 0;
                terms[i].integer.digits = data;
                terms[i].integer.len = size;
            }
        }
    }
    if (st == DLDB89_OK)
    {
        memset(&clause, 0, sizeof(clause));
        atom.predicate.data = name;
        atom.predicate.len = name_len;
        atom.arity = arity;
        atom.terms = terms;
        memset(&atom.span, 0, sizeof(atom.span));
        clause.is_rule = 0;
        clause.head = atom;
        clause.body_count = 0;
        clause.body = NULL;
        rbuf_init(&out);
        sink.write = rbuf_write;
        sink.ctx = &out;
        xst = dlx89_write_clause(&clause, &sink, &error);
        if (xst != DLX89_OK)
        {
            rbuf_free(&out);
            st = dldb89_fail(db, DLDB89_EINTERNAL, (int)xst,
                             "fact render failed");
        }
        else if (clause_add(list, out.data, out.len) != 0)
        {
            rbuf_free(&out);
            st = dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        }
        else
        {
            rbuf_free(&out);
        }
    }
    for (i = 0; i < arity; ++i)
    {
        free(payloads[i]);
    }
    free(payloads);
    free(terms);
    return st;
}

static dldb89_status collect_facts(dldb89 *db, struct clause_list *list)
{
    sqlite3_stmt *rels;
    int rc;
    dldb89_status st;

    rels = NULL;
    rc = sqlite3_prepare_v2(db->sql, "SELECT id,arity FROM dldb_relation", -1,
                            &rels, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "dump relations");
    }
    st = DLDB89_OK;
    for (;;)
    {
        sqlite3_int64 id;
        size_t arity;
        unsigned char *name;
        size_t name_len;
        sqlite3_stmt *rows;
        sqlite3_str *s;
        char *sql;
        size_t c;

        rc = sqlite3_step(rels);
        if (rc == SQLITE_DONE)
        {
            break;
        }
        if (rc != SQLITE_ROW)
        {
            st = dldb89_sql_fail(db, rc, "dump relations step");
            break;
        }
        id = sqlite3_column_int64(rels, 0);
        arity = (size_t)sqlite3_column_int(rels, 1);
        name = NULL;
        name_len = 0;
        st = fetch_relation_name(db, id, &name, &name_len);
        if (st != DLDB89_OK)
        {
            break;
        }
        s = sqlite3_str_new(db->sql);
        if (arity == 0)
        {
            sqlite3_str_appendf(s, "SELECT 1 FROM dldb_nullary"
                                   " WHERE relation=%ld AND which=0",
                                (long)id);
        }
        else
        {
            sqlite3_str_appendf(s, "SELECT ");
            for (c = 0; c < arity; ++c)
            {
                sqlite3_str_appendf(s, "%sc%d", c == 0 ? "" : ",", (int)c);
            }
            sqlite3_str_appendf(s, " FROM dldb_e%ld", (long)id);
        }
        sql = sqlite3_str_finish(s);
        if (sql == NULL)
        {
            free(name);
            st = dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
            break;
        }
        rows = NULL;
        rc = sqlite3_prepare_v2(db->sql, sql, -1, &rows, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK)
        {
            free(name);
            st = dldb89_sql_fail(db, rc, "dump rows prepare");
            break;
        }
        for (;;)
        {
            rc = sqlite3_step(rows);
            if (rc == SQLITE_DONE)
            {
                break;
            }
            if (rc != SQLITE_ROW)
            {
                st = dldb89_sql_fail(db, rc, "dump rows step");
                break;
            }
            st = render_fact_row(db, name, name_len, arity, rows, list);
            if (st != DLDB89_OK)
            {
                break;
            }
        }
        sqlite3_finalize(rows);
        free(name);
        if (st != DLDB89_OK)
        {
            break;
        }
    }
    sqlite3_finalize(rels);
    return st;
}

static dldb89_status collect_rules(dldb89 *db, struct clause_list *list)
{
    sqlite3_stmt *stmt;
    int rc;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, "SELECT dlx FROM dldb_rule", -1, &stmt,
                            NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "dump rules");
    }
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
            sqlite3_finalize(stmt);
            return dldb89_sql_fail(db, rc, "dump rules step");
        }
        blob = sqlite3_column_blob(stmt, 0);
        n = sqlite3_column_bytes(stmt, 0);
        if (clause_add(list, (const unsigned char *)blob, (size_t)n) != 0)
        {
            sqlite3_finalize(stmt);
            return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        }
    }
    sqlite3_finalize(stmt);
    return DLDB89_OK;
}

dldb89_status dldb89_dump_program(dldb89 *db, dlx89_sink *sink)
{
    struct clause_list list;
    size_t i;
    int rc;
    dldb89_status st;

    if (db == NULL || sink == NULL || sink->write == NULL)
    {
        return DLDB89_EINVAL;
    }
    dldb89_clear_error(db);
    list.items = NULL;
    list.count = 0;
    list.cap = 0;
    st = collect_facts(db, &list);
    if (st == DLDB89_OK)
    {
        st = collect_rules(db, &list);
    }
    if (st == DLDB89_OK && list.count > 1)
    {
        qsort(list.items, list.count, sizeof(*list.items), clause_cmp);
    }
    for (i = 0; st == DLDB89_OK && i < list.count; ++i)
    {
        rc = sink->write(sink->ctx, list.items[i].data, list.items[i].len);
        if (rc != 0)
        {
            st = dldb89_fail(db, DLDB89_EIO, 0, "dump sink failed");
        }
    }
    clause_list_free(&list);
    return st;
}

dldb89_status dldb89_dump(dldb89 *db, dlx89_sink *sink)
{
    if (db == NULL)
    {
        return DLDB89_EINVAL;
    }
    return dldb89_dump_program(db, sink);
}

/* ---- Integrity check ----------------------------------------------------- */

static int integer_canonical(const unsigned char *data, size_t size)
{
    size_t i;

    if (size == 0)
    {
        return 0;
    }
    i = 0;
    if (data[0] == '-')
    {
        i = 1;
        if (size == 1)
        {
            return 0;
        }
    }
    if (data[i] == '0')
    {
        return i + 1 == size && i == 0;
    }
    for (; i < size; ++i)
    {
        if (data[i] < '0' || data[i] > '9')
        {
            return 0;
        }
    }
    return 1;
}

static dldb89_status check_values(dldb89 *db)
{
    sqlite3_stmt *stmt;
    int rc;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, "SELECT kind,value FROM dldb_value", -1,
                            &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "check values");
    }
    for (;;)
    {
        int kind;
        const unsigned char *data;
        int n;

        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE)
        {
            break;
        }
        if (rc != SQLITE_ROW)
        {
            sqlite3_finalize(stmt);
            return dldb89_sql_fail(db, rc, "check values step");
        }
        kind = sqlite3_column_int(stmt, 0);
        data = (const unsigned char *)sqlite3_column_blob(stmt, 1);
        n = sqlite3_column_bytes(stmt, 1);
        if (kind != 1 && kind != 2)
        {
            sqlite3_finalize(stmt);
            return dldb89_fail(db, DLDB89_ECORRUPT, 0, "bad value kind");
        }
        if (kind == 2 && integer_canonical(data, (size_t)n) == 0)
        {
            sqlite3_finalize(stmt);
            return dldb89_fail(db, DLDB89_ECORRUPT, 0,
                               "non-canonical integer value");
        }
    }
    sqlite3_finalize(stmt);
    return DLDB89_OK;
}

static dldb89_status check_rule_text(dldb89 *db)
{
    sqlite3_stmt *stmt;
    struct rbuf buf;
    int rc;
    dlx89_program *program;
    dlx89_error error;
    dlx89_status xst;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, "SELECT dlx FROM dldb_rule", -1, &stmt,
                            NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "check rules");
    }
    rbuf_init(&buf);
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
            sqlite3_finalize(stmt);
            rbuf_free(&buf);
            return dldb89_sql_fail(db, rc, "check rules step");
        }
        blob = sqlite3_column_blob(stmt, 0);
        n = sqlite3_column_bytes(stmt, 0);
        if (rbuf_write(&buf, (const unsigned char *)blob, (size_t)n) != 0)
        {
            sqlite3_finalize(stmt);
            rbuf_free(&buf);
            return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        }
    }
    sqlite3_finalize(stmt);
    if (buf.len == 0)
    {
        rbuf_free(&buf);
        return DLDB89_OK;
    }
    program = NULL;
    xst = dlx89_parse(&program, buf.data, buf.len, NULL, &error);
    rbuf_free(&buf);
    if (xst != DLX89_OK)
    {
        return dldb89_fail(db, DLDB89_ECORRUPT, (int)xst,
                           "stored rule text invalid");
    }
    dlx89_program_destroy(program);
    return DLDB89_OK;
}

static dldb89_status check_relation_tables(dldb89 *db)
{
    sqlite3_stmt *stmt;
    int rc;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, "SELECT id,arity FROM dldb_relation", -1,
                            &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "check relations");
    }
    for (;;)
    {
        sqlite3_int64 id;
        size_t arity;
        char name[64];
        sqlite3_stmt *probe;
        int prc;
        const char *which[2];
        size_t w;

        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE)
        {
            break;
        }
        if (rc != SQLITE_ROW)
        {
            sqlite3_finalize(stmt);
            return dldb89_sql_fail(db, rc, "check relations step");
        }
        id = sqlite3_column_int64(stmt, 0);
        arity = (size_t)sqlite3_column_int(stmt, 1);
        if (arity == 0)
        {
            continue;
        }
        which[0] = "e";
        which[1] = "m";
        for (w = 0; w < 2; ++w)
        {
            sqlite3_snprintf((int)sizeof(name), name, "dldb_%s%ld", which[w],
                             (long)id);
            probe = NULL;
            prc = sqlite3_prepare_v2(db->sql,
                                     "SELECT 1 FROM sqlite_master WHERE"
                                     " type='table' AND name=?1",
                                     -1, &probe, NULL);
            if (prc != SQLITE_OK)
            {
                sqlite3_finalize(stmt);
                return dldb89_sql_fail(db, prc, "check table probe");
            }
            sqlite3_bind_text(probe, 1, name, -1, SQLITE_STATIC);
            prc = sqlite3_step(probe);
            sqlite3_finalize(probe);
            if (prc != SQLITE_ROW)
            {
                sqlite3_finalize(stmt);
                return dldb89_fail(db, DLDB89_ECORRUPT, 0,
                                   "missing physical relation table");
            }
        }
    }
    sqlite3_finalize(stmt);
    return DLDB89_OK;
}

static int check_integrity(dldb89 *db)
{
    sqlite3_stmt *stmt;
    int rc;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db->sql, "PRAGMA integrity_check", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        dldb89_sql_fail(db, rc, "integrity prepare");
        return 0;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        dldb89_fail(db, DLDB89_ECORRUPT, rc, "integrity check failed");
        return 0;
    }
    {
        const unsigned char *txt;
        int n;
        int ok;

        txt = sqlite3_column_text(stmt, 0);
        n = sqlite3_column_bytes(stmt, 0);
        ok = txt != NULL && n == 2 && memcmp(txt, "ok", 2) == 0;
        sqlite3_finalize(stmt);
        if (!ok)
        {
            dldb89_fail(db, DLDB89_ECORRUPT, 0, "integrity check not ok");
            return 0;
        }
    }
    return 1;
}

static dldb89_status checksum_materialized(dldb89 *db, unsigned long *out)
{
    sqlite3_stmt *rels;
    int rc;
    unsigned long acc;

    rels = NULL;
    rc = sqlite3_prepare_v2(db->sql, "SELECT id,arity FROM dldb_relation", -1,
                            &rels, NULL);
    if (rc != SQLITE_OK)
    {
        return dldb89_sql_fail(db, rc, "checksum relations");
    }
    acc = 0;
    for (;;)
    {
        sqlite3_int64 id;
        size_t arity;
        sqlite3_stmt *rows;
        sqlite3_str *s;
        char *sql;
        size_t c;

        rc = sqlite3_step(rels);
        if (rc == SQLITE_DONE)
        {
            break;
        }
        if (rc != SQLITE_ROW)
        {
            sqlite3_finalize(rels);
            return dldb89_sql_fail(db, rc, "checksum relations step");
        }
        id = sqlite3_column_int64(rels, 0);
        arity = (size_t)sqlite3_column_int(rels, 1);
        s = sqlite3_str_new(db->sql);
        if (arity == 0)
        {
            sqlite3_str_appendf(s, "SELECT 1 FROM dldb_nullary"
                                   " WHERE relation=%ld AND which=1",
                                (long)id);
        }
        else
        {
            sqlite3_str_appendf(s, "SELECT ");
            for (c = 0; c < arity; ++c)
            {
                sqlite3_str_appendf(s, "%sc%d", c == 0 ? "" : ",", (int)c);
            }
            sqlite3_str_appendf(s, " FROM dldb_m%ld", (long)id);
        }
        sql = sqlite3_str_finish(s);
        if (sql == NULL)
        {
            sqlite3_finalize(rels);
            return dldb89_fail(db, DLDB89_ENOMEM, 0, "out of memory");
        }
        rows = NULL;
        rc = sqlite3_prepare_v2(db->sql, sql, -1, &rows, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK)
        {
            sqlite3_finalize(rels);
            return dldb89_sql_fail(db, rc, "checksum rows prepare");
        }
        for (;;)
        {
            unsigned long h;
            size_t i;

            rc = sqlite3_step(rows);
            if (rc == SQLITE_DONE)
            {
                break;
            }
            if (rc != SQLITE_ROW)
            {
                sqlite3_finalize(rows);
                sqlite3_finalize(rels);
                return dldb89_sql_fail(db, rc, "checksum rows step");
            }
            h = 1469598103UL;
            h = (h ^ (unsigned long)id) * 16777619UL;
            for (i = 0; i < arity; ++i)
            {
                h = (h ^ (unsigned long)sqlite3_column_int64(rows, (int)i)) *
                    16777619UL;
            }
            acc = acc ^ h;
        }
        sqlite3_finalize(rows);
    }
    sqlite3_finalize(rels);
    *out = acc;
    return DLDB89_OK;
}

dldb89_status dldb89_check(dldb89 *db, int full)
{
    unsigned long pre;
    unsigned long post;
    dldb89_status st;

    if (db == NULL)
    {
        return DLDB89_EINVAL;
    }
    dldb89_clear_error(db);
    if (!check_integrity(db))
    {
        return db->error.status;
    }
    st = dldb89_schema_check(db);
    if (st != DLDB89_OK)
    {
        return st;
    }
    st = check_values(db);
    if (st != DLDB89_OK)
    {
        return st;
    }
    st = check_rule_text(db);
    if (st != DLDB89_OK)
    {
        return st;
    }
    st = check_relation_tables(db);
    if (st != DLDB89_OK)
    {
        return st;
    }
    if (full == 0)
    {
        return DLDB89_OK;
    }
    if (db->in_write != 0)
    {
        return dldb89_fail(db, DLDB89_EBUSY, 0, "write in progress");
    }
    st = dldb89_exec(db, "BEGIN IMMEDIATE;");
    if (st != DLDB89_OK)
    {
        return st;
    }
    db->in_write = 1;
    pre = 0;
    post = 0;
    st = checksum_materialized(db, &pre);
    if (st == DLDB89_OK)
    {
        st = dldb89_rebuild(db);
    }
    if (st == DLDB89_OK)
    {
        st = checksum_materialized(db, &post);
    }
    if (st == DLDB89_OK && pre != post)
    {
        st = dldb89_fail(db, DLDB89_ECORRUPT, 0,
                         "materialization is not a fixed point");
    }
    dldb89_exec(db, "ROLLBACK;");
    db->in_write = 0;
    return st;
}
