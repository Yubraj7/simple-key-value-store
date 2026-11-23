#define _POSIX_C_SOURCE 200809L
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 8192
#define MAX_ARGS 512

static int tokenize(char *line, char **argv) {
    int argc = 0;
    char *save = NULL;
    char *token = strtok_r(line, " \t\r\n", &save);
    while (token && argc < MAX_ARGS) {
        argv[argc++] = token;
        token = strtok_r(NULL, " \t\r\n", &save);
    }
    return argc;
}

int main(int argc, char **argv) {
    const char *path = "data.db";
    if (argc == 3 && strcmp(argv[1], "--db") == 0) path = argv[2];
    else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--db database-file]\n", argv[0]);
        return 2;
    }

    Database *db = db_open(path);
    if (!db) {
        fprintf(stderr, "Error: cannot open database file '%s'\n", path);
        return 1;
    }

    char line[MAX_LINE];
    char *args[MAX_ARGS];
    while (fgets(line, sizeof(line), stdin)) {
        int count = tokenize(line, args);
        if (count == 0) continue;
        if (strcmp(args[0], "EXIT") == 0) break;
        if (strcmp(args[0], "HELP") == 0) {
            db_print_help();
            continue;
        }
        db_execute(db, count, args, false);
    }

    db_close(db);
    return 0;
}
