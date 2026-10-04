/* test_reject.c - unacceptable inputs must fail atomically. */

#include <dldb89.h>

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what)
{
    fprintf(stderr, "reject: %s\n", what);
    exit(1);
}

static size_t count_rows(dldb89 *db, const char *source)
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
    if (dlq89_cursor_next_result(cursor, &result) != DLQ89_OK)
    {
        fail("query result");
    }
    count = 0;
    while (dlq89_result_next_row(result, &row) == DLQ89_OK)
    {
        count += 1;
    }
    dlq89_cursor_destroy(cursor);
    dlq89_document_destroy(doc);
    return count;
}

int main(void)
{
    const char *path = "/tmp/dldb89_reject.db";
    const char *text_path = "/tmp/dldb89_reject.txt";
    const char *good = "p(\"a\").\np(\"b\").\n";
    const char *bad_dlx = "p(X).\n";
    dldb89 *db;
    dldb89 *db2;
    dldb89_status st;
    dlx89_program *program;
    dlx89_error error;
    dlx89_status xst;
    unsigned long generation;
    FILE *f;

    remove(path);
    remove(text_path);

    /* A plain text file is not a dldb database. */
    f = fopen(text_path, "wb");
    if (f == NULL)
    {
        fail("write text file");
    }
    fputs("not a database\n", f);
    fclose(f);

    db = NULL;
    st = dldb89_create(&db, path, NULL);
    if (st != DLDB89_OK)
    {
        fail("create");
    }
    db2 = NULL;
    st = dldb89_create(&db2, path, NULL);
    if (st != DLDB89_EDATABASE)
    {
        fail("create over existing must fail");
    }
    if (db2 != NULL)
    {
        fail("failed create must not return a handle");
    }
    st = dldb89_load_text(db, good, strlen(good));
    if (st != DLDB89_OK)
    {
        fail("good load");
    }
    if (count_rows(db, "?- p(X).") != 2)
    {
        fail("good count");
    }

    /* Invalid DLX rejects the whole load and leaves the database unchanged. */
    generation = dldb89_generation(db);
    st = dldb89_load_text(db, bad_dlx, strlen(bad_dlx));
    if (st != DLDB89_EPROGRAM)
    {
        fail("invalid DLX must be EPROGRAM");
    }
    if (dldb89_generation(db) != generation)
    {
        fail("failed load advanced generation");
    }
    if (count_rows(db, "?- p(X).") != 2)
    {
        fail("failed load changed data");
    }

    /* replace_rules rejects a program containing a fact. */
    program = NULL;
    xst = dlx89_parse(&program, "q(\"z\").\n", strlen("q(\"z\").\n"), NULL,
                      &error);
    if (xst != DLX89_OK)
    {
        fail("parse fact program");
    }
    st = dldb89_replace_rules(db, program);
    dlx89_program_destroy(program);
    if (st != DLDB89_EPROGRAM)
    {
        fail("replace_rules with fact must be EPROGRAM");
    }
    if (dldb89_generation(db) != generation)
    {
        fail("failed replace advanced generation");
    }
    if (count_rows(db, "?- p(X).") != 2)
    {
        fail("failed replace changed data");
    }
    dldb89_close(db);

    /* Opening a non-database fails cleanly. */
    db = NULL;
    st = dldb89_open(&db, text_path, NULL);
    if (st != DLDB89_EDATABASE && st != DLDB89_EFORMAT)
    {
        fail("non-database must fail");
    }
    if (db != NULL)
    {
        fail("failed open must not return a handle");
    }

    /* A SQLite file with an unknown dldb format is EFORMAT. */
    {
        sqlite3 *raw;
        char *err;
        int rc;

        remove(path);
        raw = NULL;
        rc = sqlite3_open(path, &raw);
        err = NULL;
        if (rc == SQLITE_OK)
        {
            rc = sqlite3_exec(raw,
                              "CREATE TABLE dldb_meta (key TEXT PRIMARY KEY,"
                              " value BLOB NOT NULL);"
                              "INSERT INTO dldb_meta VALUES('format','999');",
                              NULL, NULL, &err);
        }
        sqlite3_free(err);
        sqlite3_close(raw);
        if (rc != SQLITE_OK)
        {
            fail("prepare bad format db");
        }
        db = NULL;
        st = dldb89_open(&db, path, NULL);
        if (st != DLDB89_EFORMAT)
        {
            fail("unknown format must be EFORMAT");
        }
        if (db != NULL)
        {
            fail("format failure must not return a handle");
        }
    }
    remove(path);
    remove(text_path);
    return 0;
}
