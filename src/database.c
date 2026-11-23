#define _POSIX_C_SOURCE 200809L
#include "database.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool quiet_mode = false;
#define printf(...) do { if (!quiet_mode) printf(__VA_ARGS__); } while (0)
#define puts(s) (quiet_mode ? 0 : puts(s))

#define MAX_LOG_LINE 8192

typedef struct Field {
    char *key, *value;
    struct Field *next;
} Field;

typedef struct Item {
    char *value;
    struct Item *next;
} Item;

typedef struct Entry {
    char *key;
    char *value;
    long long expires_at; /* 0 means no expiration */
    Field *fields;
    Item *head, *tail;
    size_t list_len;
    struct Entry *next;
} Entry;

typedef struct Pending {
    char *line;
    struct Pending *next;
} Pending;

struct Database {
    FILE *log;
    char *path;
    Entry *entries;       /* Deliberately a linked-list index, not a map. */
    bool in_transaction;
    Pending *pending_head, *pending_tail;
};

static void free_fields(Field *f) {
    while (f) { Field *n = f->next; free(f->key); free(f->value); free(f); f = n; }
}
static void free_items(Item *i) {
    while (i) { Item *n = i->next; free(i->value); free(i); i = n; }
}
static void free_entry(Entry *e) {
    if (!e) return;
    free(e->key); free(e->value); free_fields(e->fields); free_items(e->head); free(e);
}
static void clear_entries(Database *db) {
    Entry *e = db->entries;
    while (e) { Entry *n = e->next; free_entry(e); e = n; }
    db->entries = NULL;
}
static Entry *find_entry(Database *db, const char *key) {
    for (Entry *e = db->entries; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;
    return NULL;
}
static Entry *get_or_create(Database *db, const char *key) {
    Entry *e = find_entry(db, key);
    if (e) return e;
    e = calloc(1, sizeof(*e));
    if (!e || !(e->key = strdup(key))) { free(e); return NULL; }
    e->next = db->entries; db->entries = e;
    return e;
}
static bool expired(Entry *e) {
    return e && e->expires_at > 0 && (long long)time(NULL) >= e->expires_at;
}
static void delete_entry(Database *db, const char *key) {
    Entry **p = &db->entries;
    while (*p) {
        if (strcmp((*p)->key, key) == 0) {
            Entry *old = *p; *p = old->next; free_entry(old); return;
        }
        p = &(*p)->next;
    }
}
static void purge_expired(Database *db) {
    Entry *e = db->entries, *next;
    while (e) { next = e->next; if (expired(e)) delete_entry(db, e->key); e = next; }
}
static void clear_pending(Database *db) {
    Pending *p = db->pending_head;
    while (p) { Pending *n = p->next; free(p->line); free(p); p = n; }
    db->pending_head = db->pending_tail = NULL;
}
static int append_line(Database *db, const char *line) {
    if (fputs(line, db->log) == EOF || fputc('\n', db->log) == EOF ||
        fflush(db->log) != 0) return -1;
    return 0;
}
static int persist_or_queue(Database *db, int argc, char **argv, bool replay) {
    if (replay) return 0;
    size_t n = 1;
    for (int i = 0; i < argc; i++) n += strlen(argv[i]) + 1;
    char *line = malloc(n + 1);
    if (!line) return -1;
    line[0] = '\0';
    for (int i = 0; i < argc; i++) {
        if (i) strcat(line, " ");
        strcat(line, argv[i]);
    }
    if (db->in_transaction) {
        Pending *p = calloc(1, sizeof(*p));
        if (!p) { free(line); return -1; }
        p->line = line;
        if (db->pending_tail) db->pending_tail->next = p;
        else db->pending_head = p;
        db->pending_tail = p;
        return 0;
    }
    int rc = append_line(db, line);
    free(line);
    return rc;
}
static int integer_arg(const char *s, long long *out) {
    char *end = NULL; errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || !end || *end) return -1;
    *out = v; return 0;
}
static Field *find_field(Entry *e, const char *key) {
    for (Field *f = e->fields; f; f = f->next)
        if (strcmp(f->key, key) == 0) return f;
    return NULL;
}
static int set_field(Entry *e, const char *key, const char *value) {
    Field *f = find_field(e, key);
    if (!f) {
        f = calloc(1, sizeof(*f));
        if (!f || !(f->key = strdup(key))) { free(f); return -1; }
        f->next = e->fields; e->fields = f;
    }
    char *v = strdup(value);
    if (!v) return -1;
    free(f->value); f->value = v; return 0;
}
static int set_scalar(Database *db, const char *key, const char *value) {
    Entry *e = get_or_create(db, key);
    if (!e) return -1;
    char *v = strdup(value);
    if (!v) return -1;
    free(e->value); e->value = v;
    e->expires_at = 0;
    free_fields(e->fields); e->fields = NULL;
    free_items(e->head); e->head = e->tail = NULL; e->list_len = 0;
    return 0;
}
static void print_value(Entry *e) {
    if (!e || !e->value) puts("(nil)");
    else puts(e->value);
}
static int command_error(const char *message) {
    printf("ERR %s\n", message); return -1;
}
static int require_count(int argc, int min, int max) {
    if (argc < min || argc > max) { puts("ERR wrong number of arguments"); return 0; }
    return 1;
}
static int execute_internal(Database *db, int argc, char **argv, bool replay);

static int execute_internal(Database *db, int argc, char **argv, bool replay) {
    if (argc == 0) return 0;
    const char *cmd = argv[0];
    purge_expired(db);

    if (!strcmp(cmd, "SET")) {
        if (!require_count(argc, 3, 3)) return -1;
        if (persist_or_queue(db, argc, argv, replay) || set_scalar(db, argv[1], argv[2]))
            return command_error("write failed");
        puts("OK"); return 0;
    }
    if (!strcmp(cmd, "GET")) {
        if (!require_count(argc, 2, 2)) return -1;
        print_value(find_entry(db, argv[1])); return 0;
    }
    if (!strcmp(cmd, "DEL")) {
        if (!require_count(argc, 2, 2)) return -1;
        int existed = find_entry(db, argv[1]) != NULL;
        if (persist_or_queue(db, argc, argv, replay)) return command_error("write failed");
        delete_entry(db, argv[1]); printf("%d\n", existed); return 0;
    }
    if (!strcmp(cmd, "EXISTS")) {
        if (!require_count(argc, 2, 2)) return -1;
        printf("%d\n", find_entry(db, argv[1]) != NULL); return 0;
    }
    if (!strcmp(cmd, "MSET")) {
        if (argc < 3 || argc % 2 == 0) return command_error("MSET expects key/value pairs");
        if (persist_or_queue(db, argc, argv, replay)) return command_error("write failed");
        for (int i = 1; i < argc; i += 2)
            if (set_scalar(db, argv[i], argv[i+1])) return command_error("out of memory");
        puts("OK"); return 0;
    }
    if (!strcmp(cmd, "MGET")) {
        if (!require_count(argc, 2, MAX_LOG_LINE)) return -1;
        for (int i = 1; i < argc; i++) print_value(find_entry(db, argv[i]));
        return 0;
    }
    if (!strcmp(cmd, "EXPIRE")) {
        long long seconds;
        if (!require_count(argc, 3, 3) || integer_arg(argv[2], &seconds) || seconds < 0)
            return command_error("EXPIRE expects a non-negative number of seconds");
        Entry *e = find_entry(db, argv[1]);
        if (!e) { puts("0"); return 0; }
        char *logargs[] = {"EXPIREAT", argv[1], NULL};
        char timestamp[32];
        snprintf(timestamp, sizeof(timestamp), "%lld", (long long)time(NULL) + seconds);
        logargs[2] = timestamp;
        if (persist_or_queue(db, 3, logargs, replay)) return command_error("write failed");
        e->expires_at = (long long)time(NULL) + seconds;
        puts("1"); return 0;
    }
    if (!strcmp(cmd, "EXPIREAT")) {
        long long when;
        if (argc != 3 || integer_arg(argv[2], &when)) return -1;
        Entry *e = find_entry(db, argv[1]); if (e) e->expires_at = when;
        return 0;
    }
    if (!strcmp(cmd, "TTL")) {
        if (!require_count(argc, 2, 2)) return -1;
        Entry *e = find_entry(db, argv[1]);
        if (!e) puts("-2");
        else if (!e->expires_at) puts("-1");
        else { long long left = e->expires_at - (long long)time(NULL); printf("%lld\n", left > 0 ? left : 0); }
        return 0;
    }
    if (!strcmp(cmd, "RANGE")) {
        if (!require_count(argc, 3, 3)) return -1;
        /* Linked-list index is scanned; output is lexicographically sorted. */
        size_t count = 0;
        for (Entry *e = db->entries; e; e = e->next)
            if (strcmp(e->key, argv[1]) >= 0 && strcmp(e->key, argv[2]) <= 0) count++;
        Entry **arr = malloc((count ? count : 1) * sizeof(*arr));
        if (!arr) return command_error("out of memory");
        size_t j = 0;
        for (Entry *e = db->entries; e; e = e->next)
            if (strcmp(e->key, argv[1]) >= 0 && strcmp(e->key, argv[2]) <= 0) arr[j++] = e;
        for (size_t a = 0; a < count; a++)
            for (size_t b = a + 1; b < count; b++)
                if (strcmp(arr[a]->key, arr[b]->key) > 0) { Entry *t=arr[a]; arr[a]=arr[b]; arr[b]=t; }
        for (size_t a = 0; a < count; a++) printf("%s %s\n", arr[a]->key, arr[a]->value ? arr[a]->value : "(nil)");
        free(arr); return 0;
    }
    if (!strcmp(cmd, "BEGIN")) {
        if (!require_count(argc, 1, 1)) return -1;
        if (db->in_transaction) return command_error("transaction already active");
        db->in_transaction = true; clear_pending(db); puts("OK"); return 0;
    }
    if (!strcmp(cmd, "COMMIT")) {
        if (!require_count(argc, 1, 1)) return -1;
        if (!db->in_transaction) return command_error("no active transaction");
        for (Pending *p = db->pending_head; p; p = p->next)
            if (append_line(db, p->line)) return command_error("commit failed");
        db->in_transaction = false; clear_pending(db); puts("OK"); return 0;
    }
    if (!strcmp(cmd, "ABORT")) {
        if (!require_count(argc, 1, 1)) return -1;
        if (!db->in_transaction) return command_error("no active transaction");
        /* Rebuild committed state, discarding the uncommitted in-memory changes. */
        clear_entries(db);
        quiet_mode = true;
        rewind(db->log);
        char line[MAX_LOG_LINE];
        while (fgets(line, sizeof(line), db->log)) {
            char *a[512]; int n=0; char *save=NULL;
            for (char *t=strtok_r(line," \t\r\n",&save); t && n<512; t=strtok_r(NULL," \t\r\n",&save)) a[n++]=t;
            execute_internal(db,n,a,true);
        }
        fseek(db->log, 0, SEEK_END);
        quiet_mode = false;
        db->in_transaction = false; clear_pending(db); puts("OK"); return 0;
    }
    if (!strcmp(cmd, "HSET")) {
        if (!require_count(argc, 4, 4)) return -1;
        if (persist_or_queue(db, argc, argv, replay)) return command_error("write failed");
        Entry *e = get_or_create(db, argv[1]);
        if (!e || set_field(e, argv[2], argv[3])) return command_error("out of memory");
        puts("1"); return 0;
    }
    if (!strcmp(cmd, "HGET")) {
        if (!require_count(argc, 3, 3)) return -1;
        Entry *e=find_entry(db,argv[1]); Field *f=e?find_field(e,argv[2]):NULL;
        if (f) puts(f->value); else puts("(nil)"); return 0;
    }
    if (!strcmp(cmd, "HGETALL")) {
        if (!require_count(argc, 2, 2)) return -1;
        Entry *e=find_entry(db,argv[1]);
        if (!e || !e->fields) { puts("(empty)"); return 0; }
        for (Field *f=e->fields; f; f=f->next) printf("%s %s\n",f->key,f->value);
        return 0;
    }
    if (!strcmp(cmd, "LPUSH") || !strcmp(cmd, "RPUSH")) {
        if (!require_count(argc, 3, 3)) return -1;
        if (persist_or_queue(db,argc,argv,replay)) return command_error("write failed");
        Entry *e=get_or_create(db,argv[1]); Item *it=calloc(1,sizeof(*it));
        if (!e || !it || !(it->value=strdup(argv[2]))) { free(it); return command_error("out of memory"); }
        if (!strcmp(cmd,"LPUSH")) { it->next=e->head; e->head=it; if (!e->tail) e->tail=it; }
        else { if (e->tail) e->tail->next=it; else e->head=it; e->tail=it; }
        e->list_len++; printf("%zu\n",e->list_len); return 0;
    }
    if (!strcmp(cmd, "LRANGE")) {
        long long start, stop;
        if (!require_count(argc,4,4) || integer_arg(argv[2],&start) || integer_arg(argv[3],&stop))
            return command_error("LRANGE expects integer indexes");
        Entry *e=find_entry(db,argv[1]); if (!e) { puts("(empty)"); return 0; }
        long long len=(long long)e->list_len;
        if (start < 0) start += len;
        if (stop < 0) stop += len;
        if (start < 0) start = 0;
        if (stop >= len) stop = len - 1;
        if (start>stop || start>=len) { puts("(empty)"); return 0; }
        long long i=0; for (Item *it=e->head; it; it=it->next,i++)
            if (i>=start && i<=stop) puts(it->value);
        return 0;
    }
    if (!strcmp(cmd, "INCR") || !strcmp(cmd, "DECR")) {
        if (!require_count(argc,2,2)) return -1;
        Entry *e=find_entry(db,argv[1]); long long value=0;
        if (e && e->value && integer_arg(e->value,&value)) return command_error("value is not an integer");
        if ((!strcmp(cmd,"INCR") && value==LLONG_MAX) || (!strcmp(cmd,"DECR") && value==LLONG_MIN))
            return command_error("integer overflow");
        value += !strcmp(cmd,"INCR") ? 1 : -1;
        char number[64]; snprintf(number,sizeof(number),"%lld",value);
        char *logargs[]={ "SET", argv[1], number };
        if (persist_or_queue(db,3,logargs,replay) || set_scalar(db,argv[1],number)) return command_error("write failed");
        printf("%lld\n",value); return 0;
    }
    if (!strcmp(cmd, "FLUSHDB")) {
        if (!require_count(argc,1,1)) return -1;
        if (persist_or_queue(db,argc,argv,replay)) return command_error("write failed");
        clear_entries(db); puts("OK"); return 0;
    }
    if (!strcmp(cmd, "HELP")) { db_print_help(); return 0; }
    return command_error("unknown command (try HELP)");
}

int db_execute(Database *db, int argc, char **argv, bool replay) {
    return execute_internal(db, argc, argv, replay);
}

Database *db_open(const char *path) {
    Database *db = calloc(1,sizeof(*db));
    if (!db) return NULL;
    db->path=strdup(path);
    db->log=fopen(path,"a+");
    if (!db->path || !db->log) { free(db->path); free(db); return NULL; }
    rewind(db->log);
    quiet_mode = true;
    char line[MAX_LOG_LINE];
    while (fgets(line,sizeof(line),db->log)) {
        char *args[512]; int argc=0; char *save=NULL;
        for (char *t=strtok_r(line," \t\r\n",&save); t && argc<512; t=strtok_r(NULL," \t\r\n",&save)) args[argc++]=t;
        if (argc) execute_internal(db,argc,args,true);
    }
    fseek(db->log,0,SEEK_END);
    quiet_mode = false;
    return db;
}
void db_close(Database *db) {
    if (!db) return;
    clear_pending(db); clear_entries(db);
    if (db->log) fclose(db->log);
    free(db->path); free(db);
}
void db_print_help(void) {
    puts("Commands: SET key value | GET key | DEL key | EXISTS key | MSET k v [k v ...]");
    puts("MGET key [key ...] | EXPIRE key seconds | TTL key | RANGE start end");
    puts("BEGIN | COMMIT | ABORT | HSET hash field value | HGET hash field | HGETALL hash");
    puts("LPUSH key value | RPUSH key value | LRANGE key start stop | INCR key | DECR key | FLUSHDB | EXIT");
}
