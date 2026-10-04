#ifndef DLDB89_H
#define DLDB89_H

/* dldb89.h - persistent finite positive Datalog database.
 *
 * dldb persists asserted facts and canonical DLX rules in SQLite, materializes
 * the least fixed point through libdatalog89, and exposes snapshot-consistent
 * conjunctive queries through the libdlq89 database adapter.
 *
 * Every committed generation satisfies
 *
 *     M = lfp(EDB, Rules)
 *
 * There is no dirty state: a mutation either commits a complete fixed point or
 * leaves the previous generation untouched.
 */

#include <stddef.h>

#include <dlq89.h>
#include <dlx89.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dldb89 dldb89;

/* Status of a fallible operation. DLDB89_OK is zero, DLDB89_END is the
 * positive iteration terminator, and every failure is negative. */
typedef enum dldb89_status
{
    DLDB89_OK = 0,
    DLDB89_END = 1,
    DLDB89_ENOTFOUND = 2,
    DLDB89_EINVAL = -1,
    DLDB89_ENOMEM = -2,
    DLDB89_EIO = -3,
    DLDB89_EDATABASE = -4,
    DLDB89_EPROGRAM = -5,
    DLDB89_EQUERY = -6,
    DLDB89_EBUSY = -7,
    DLDB89_EFORMAT = -8,
    DLDB89_ECORRUPT = -9,
    DLDB89_EINTERNAL = -10
} dldb89_status;

/* Structured error. message points into the database handle and remains valid
 * until the next dldb89 call on that handle. system_error is the raw SQLite
 * result code when relevant; sub_status is the originating library status
 * (dlx89, dlq89, or datalog89) when relevant. */
typedef struct dldb89_error
{
    dldb89_status status;
    int system_error;
    int sub_status;
    const char *message;
} dldb89_error;

typedef struct dldb89_options
{
    size_t max_arity;      /* 0 selects the default cap */
    int synchronous_full;  /* nonzero selects PRAGMA synchronous=FULL */
} dldb89_options;

void dldb89_options_init(dldb89_options *options);
const char *dldb89_status_name(dldb89_status status);
const char *dldb89_status_message(dldb89_status status);

/* Create a fresh database at path, failing if it already exists. */
dldb89_status dldb89_create(dldb89 **out, const char *path,
                            const dldb89_options *options);

/* Open an existing database; validates the on-disk format. */
dldb89_status dldb89_open(dldb89 **out, const char *path,
                          const dldb89_options *options);

void dldb89_close(dldb89 *db);
const dldb89_error *dldb89_last_error(const dldb89 *db);
unsigned long dldb89_generation(const dldb89 *db);

/* Additively load facts and rules from a validated DLX program and commit the
 * resulting least fixed point atomically. Duplicate facts and rules are
 * no-ops. On any failure the database is unchanged. */
dldb89_status dldb89_load_program(dldb89 *db, const dlx89_program *program);

/* Parse data as DLX and load it. On a parse failure the database is
 * unchanged. */
dldb89_status dldb89_load_text(dldb89 *db, const void *data, size_t size);

/* Replace the entire rule set while preserving asserted EDB facts, then commit
 * the resulting least fixed point. A program clause that is a fact is
 * rejected with DLDB89_EPROGRAM. */
dldb89_status dldb89_replace_rules(dldb89 *db, const dlx89_program *program);

/* Emit the canonical DLX program of all asserted facts and persistent rules,
 * sorted by canonical bytes. Derived facts are not included. */
dldb89_status dldb89_dump(dldb89 *db, dlx89_sink *sink);

/* Verify structural integrity. When full is nonzero, additionally rebuild the
 * least fixed point in a rolled-back transaction and require it to match the
 * stored materialization. */
dldb89_status dldb89_check(dldb89 *db, int full);

/* The DLQ database adapter over this database. The returned pointer is owned by
 * db and valid until dldb89_close. A query cursor must be destroyed before the
 * next mutating call on db. */
const dlq89_db *dldb89_query_db(dldb89 *db);

#ifdef __cplusplus
}
#endif

#endif /* DLDB89_H */
