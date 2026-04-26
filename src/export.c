#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "crag.h"
#include "store.h"
#include "util.h"

/* Dump every chunk as one JSON object per line: {"source":"...","text":"..."}.
   Vectors are intentionally excluded — the whole point of export/import is to
   move a corpus across embedders. The caller re-embeds on import. */
int cmd_export(int argc, char **argv) {
    const char *db_path = NULL;
    const char *out_path = NULL;
    for (int i = 0; i < argc; i++) {
        if      ((strcmp(argv[i], "--db")  == 0 || strcmp(argv[i], "-i") == 0) && i+1 < argc) db_path  = argv[++i];
        else if ((strcmp(argv[i], "--out") == 0 || strcmp(argv[i], "-o") == 0) && i+1 < argc) out_path = argv[++i];
    }
    if (!db_path) db_path = getenv("CRAG_DB");
    if (!db_path) db_path = "./crag.db";

    FILE *fp = stdout;
    if (out_path) {
        fp = fopen(out_path, "w");
        if (!fp) { fprintf(stderr, "export: cannot open '%s' for write\n", out_path); return 3; }
    }

    crag_store_t store;
    if (store_open(db_path, &store) != 0) {
        if (fp != stdout) fclose(fp);
        return 3;
    }

    sqlite3_stmt *stmt;
    const char *sql = "SELECT source, text FROM chunks ORDER BY id";
    if (sqlite3_prepare_v2(store.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "export: query failed\n");
        store_close(&store);
        if (fp != stdout) fclose(fp);
        return 1;
    }

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *src = (const char *)sqlite3_column_text(stmt, 0);
        const char *txt = (const char *)sqlite3_column_text(stmt, 1);
        char *esrc = json_escape_dup(src ? src : "");
        char *etxt = json_escape_dup(txt ? txt : "");
        if (esrc && etxt)
            fprintf(fp, "{\"source\":\"%s\",\"text\":\"%s\"}\n", esrc, etxt);
        free(esrc); free(etxt);
        n++;
    }
    sqlite3_finalize(stmt);

    fprintf(stderr, "export: wrote %d chunks to %s\n", n, out_path ? out_path : "stdout");

    store_close(&store);
    if (fp != stdout) fclose(fp);
    return 0;
}
