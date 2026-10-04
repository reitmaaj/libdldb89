/* test_smoke.c - end-to-end dldb89 lifecycle. */

#include <dldb89.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what)
{
    fprintf(stderr, "smoke: %s\n", what);
    exit(1);
}

static void run_count(dldb89 *db, const char *source, size_t want)
{
    dlq89_document *doc;
    dlq89_cursor *cursor;
    dlq89_result *result;
    const dlq89_value *row;
    dlq89_diagnostic diag;
    size_t count;

    doc = NULL;
    if (dlq89_parse((const unsigned char *)source, strlen(source), &doc,
                    &diag) != DLQ89_OK)
    {
        fail("query parse");
    }
    cursor = NULL;
    if (dlq89_execute(doc, dldb89_query_db(db), &cursor, &diag) != DLQ89_OK)
    {
        fail("query execute");
    }
    result = NULL;
    if (dlq89_cursor_next_result(cursor, &result) != DLQ89_OK ||
        dlq89_result_kindof(result) != DLQ89_TABLE)
    {
        fail("query result kind");
    }
    count = 0;
    while (dlq89_result_next_row(result, &row) == DLQ89_OK)
    {
        count += 1;
    }
    if (count != want)
    {
        fail("query row count");
    }
    dlq89_cursor_destroy(cursor);
    dlq89_document_destroy(doc);
}

static void run_bool(dldb89 *db, const char *source, int want)
{
    dlq89_document *doc;
    dlq89_cursor *cursor;
    dlq89_result *result;
    dlq89_diagnostic diag;
    int got;

    doc = NULL;
    if (dlq89_parse((const unsigned char *)source, strlen(source), &doc,
                    &diag) != DLQ89_OK)
    {
        fail("bool parse");
    }
    cursor = NULL;
    if (dlq89_execute(doc, dldb89_query_db(db), &cursor, &diag) != DLQ89_OK)
    {
        fail("bool execute");
    }
    result = NULL;
    if (dlq89_cursor_next_result(cursor, &result) != DLQ89_OK ||
        dlq89_result_kindof(result) != DLQ89_BOOLEAN)
    {
        fail("bool result kind");
    }
    got = dlq89_result_boolean(result);
    if (got != want)
    {
        fail("bool value");
    }
    dlq89_cursor_destroy(cursor);
    dlq89_document_destroy(doc);
}

static char dump_buf[8192];
static size_t dump_len;

static int dump_sink(void *ctx, const unsigned char *bytes, size_t len)
{
    (void)ctx;
    if (dump_len + len >= sizeof(dump_buf))
    {
        return 1;
    }
    memcpy(dump_buf + dump_len, bytes, len);
    dump_len += len;
    dump_buf[dump_len] = '\0';
    return 0;
}

int main(void)
{
    const char *path = "/tmp/dldb89_smoke.db";
    const char *program =
        "parent(\"alice\", \"bob\").\n"
        "parent(\"bob\", \"carol\").\n"
        "ancestor(X, Y) :- parent(X, Y).\n"
        "ancestor(X, Z) :- ancestor(X, Y), parent(Y, Z).\n"
        "n(42).\n"
        "n(\"42\").\n";
    dldb89 *db;
    dldb89_status st;
    dlx89_status xst;
    dlx89_program *rules;
    dlx89_error error;
    dlx89_sink sink;

    remove(path);
    db = NULL;
    st = dldb89_create(&db, path, NULL);
    if (st != DLDB89_OK)
    {
        fail("create");
    }
    st = dldb89_load_text(db, program, strlen(program));
    if (st != DLDB89_OK)
    {
        fprintf(stderr, "load: %s\n", dldb89_status_name(st));
        fail("load");
    }
    run_count(db, "?- ancestor(\"alice\", X).", 2);
    run_count(db, "?- n(X).", 2);
    run_bool(db, "?- n(42).", 1);
    run_bool(db, "?- n(\"42\").", 1);
    run_bool(db, "?- n(43).", 0);

    dump_len = 0;
    dump_buf[0] = '\0';
    sink.write = dump_sink;
    sink.ctx = NULL;
    st = dldb89_dump(db, &sink);
    if (st != DLDB89_OK)
    {
        fail("dump");
    }
    if (strstr(dump_buf, "ancestor(X, Y) :- parent(X, Y).\n") == NULL)
    {
        fail("dump missing rule");
    }
    if (strstr(dump_buf, "parent(\"alice\", \"bob\").\n") == NULL)
    {
        fail("dump missing fact");
    }
    if (strstr(dump_buf, "n(42).\n") == NULL)
    {
        fail("dump missing integer fact");
    }

    if (dldb89_check(db, 1) != DLDB89_OK)
    {
        fail("check full");
    }
    dldb89_close(db);

    db = NULL;
    st = dldb89_open(&db, path, NULL);
    if (st != DLDB89_OK)
    {
        fail("reopen");
    }
    run_count(db, "?- ancestor(\"alice\", X).", 2);
    if (dldb89_generation(db) == 0)
    {
        fail("generation not persisted");
    }

    rules = NULL;
    xst = dlx89_parse(&rules, "ancestor(X, Y) :- parent(X, Y).\n",
                      strlen("ancestor(X, Y) :- parent(X, Y).\n"), NULL,
                      &error);
    if (xst != DLX89_OK)
    {
        fail("rules parse");
    }
    st = dldb89_replace_rules(db, rules);
    dlx89_program_destroy(rules);
    if (st != DLDB89_OK)
    {
        fail("replace rules");
    }
    run_count(db, "?- ancestor(\"alice\", X).", 1);
    if (dldb89_check(db, 1) != DLDB89_OK)
    {
        fail("check full after replace");
    }
    dldb89_close(db);
    remove(path);
    return 0;
}
