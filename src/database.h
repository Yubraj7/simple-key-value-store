#ifndef DATABASE_H
#define DATABASE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct Database Database;

Database *db_open(const char *path);
void db_close(Database *db);
int db_execute(Database *db, int argc, char **argv, bool replay);
void db_print_help(void);

#endif
