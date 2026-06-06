#include "app.h"
#include <stdio.h>
#include <sqlite3.h>

static sqlite3 *db = NULL;
static unsigned int inserts_since_cleanup = 0;
int chat_db_init(const char *path) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "message TEXT NOT NULL,"
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");";
    char *err = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "sqlite open failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_busy_timeout(db, 100);
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sqlite schema error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

void chat_db_close(void) {
    if (db) sqlite3_close(db);
    db = NULL;
}

int chat_db_insert_message(const char *name, const char *message) {
    sqlite3_stmt *stmt = NULL;
    if (!db || sqlite3_prepare_v2(db, "INSERT INTO messages(name, message) VALUES(?, ?);", -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, message, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok && ++inserts_since_cleanup >= 100) {
        sqlite3_stmt *cleanup = NULL;
        if (sqlite3_prepare_v2(
                db,
                "DELETE FROM messages WHERE id <= (SELECT MAX(id) - ? FROM messages);",
                -1,
                &cleanup,
                NULL
            ) == SQLITE_OK) {
            sqlite3_bind_int(cleanup, 1, MAX_STORED_MESSAGES);
            sqlite3_step(cleanup);
        }
        sqlite3_finalize(cleanup);
        inserts_since_cleanup = 0;
    }
    return ok ? 0 : -1;
}

int chat_db_send_recent_messages(int fd, int limit) {
    const char *sql =
        "SELECT name, message FROM ("
        "SELECT id, name, message FROM messages ORDER BY id DESC LIMIT ?"
        ") ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    if (!db || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, limit);
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return -1;
        }
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *msg = (const char *)sqlite3_column_text(stmt, 1);
        char line[MAX_NAME_LEN + 2 + MAX_WS_PAYLOAD + 1];
        snprintf(line, sizeof(line), "%s: %s", name ? name : "anon", msg ? msg : "");
        if (ws_send_text(fd, line) < 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }
    sqlite3_finalize(stmt);
    return 0;
}
