#ifndef DLDB89_PRIV_H
#define DLDB89_PRIV_H

#include <dldb89.h>

#include <datalog89.h>
#include <sqlite3.h>

#define DLDB89_FORMAT 1
#define DLDB89_DEFAULT_MAX_ARITY 64
#define DLDB89_ERRBUF 512

struct dldb89
{
    sqlite3 *sql;
    char *path;
    dldb89_options options;
    dldb89_error error;
    char error_buf[DLDB89_ERRBUF];
    int in_write;
    unsigned long generation;
    dlq89_db query_db;
};

/* ---- Errors -------------------------------------------------------------- */

void dldb89_clear_error(dldb89 *db);
dldb89_status dldb89_fail(dldb89 *db, dldb89_status status, int sub_status,
                          const char *message);
dldb89_status dldb89_sql_fail(dldb89 *db, int rc, const char *what);

/* ---- SQL helpers --------------------------------------------------------- */

dldb89_status dldb89_exec(dldb89 *db, const char *sql);
dldb89_status dldb89_schema_create(dldb89 *db);
dldb89_status dldb89_schema_check(dldb89 *db);

/* ---- Values and relations ------------------------------------------------ */

dldb89_status dldb89_value_intern(dldb89 *db, int kind, const void *data,
                                  size_t size, sqlite3_int64 *out_id);
dldb89_status dldb89_value_fetch(dldb89 *db, sqlite3_int64 id, int *kind,
                                 unsigned char **out_data, size_t *out_size);

dldb89_status dldb89_relation_intern(dldb89 *db, const unsigned char *name,
                                     size_t name_size, size_t arity,
                                     sqlite3_int64 *out_id);
dldb89_status dldb89_relation_lookup(dldb89 *db, const unsigned char *name,
                                     size_t name_size, size_t arity,
                                     sqlite3_int64 *out_id);
dldb89_status dldb89_relation_arity(dldb89 *db, sqlite3_int64 id,
                                    size_t *out_arity);
dldb89_status dldb89_relation_table(dldb89 *db, sqlite3_int64 id, int material,
                                    char *buf, size_t cap);

/* ---- Ingestion and materialization --------------------------------------- */

/* mode: 0 = add facts and rules, 1 = replace rules (facts rejected). */
dldb89_status dldb89_ingest(dldb89 *db, const dlx89_program *program, int mode);
dldb89_status dldb89_rebuild(dldb89 *db);

/* ---- Dump and integrity -------------------------------------------------- */

dldb89_status dldb89_dump_program(dldb89 *db, dlx89_sink *sink);

/* ---- Query adapter ------------------------------------------------------- */

const dlq89_db_ops *dldb89_query_ops(void);

#endif /* DLDB89_PRIV_H */
