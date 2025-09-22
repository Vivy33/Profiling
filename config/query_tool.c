#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

// --- 内置哈希表实现 ---

#define HASH_TABLE_SIZE 4096

typedef struct ht_entry {
    char *key;
    int value;
    struct ht_entry *next;
} ht_entry_t;

typedef struct {
    ht_entry_t **entries;
} ht_t;

// djb2 哈希函数
static unsigned int hash(const char *key) {
    unsigned long int hash = 5381;
    int c;
    while ((c = *key++))
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    return hash % HASH_TABLE_SIZE;
}

static ht_t* ht_create(void) {
    ht_t *hashtable = malloc(sizeof(ht_t));
    hashtable->entries = calloc(HASH_TABLE_SIZE, sizeof(ht_entry_t*));
    return hashtable;
}

static void ht_insert(ht_t *hashtable, const char *key, int value) {
    unsigned int slot = hash(key);
    ht_entry_t *entry = hashtable->entries[slot];

    // 查找现有条目
    while (entry != NULL) {
        if (strcmp(entry->key, key) == 0) {
            entry->value += value;
            return;
        }
        entry = entry->next;
    }

    // 创建新条目
    entry = malloc(sizeof(ht_entry_t));
    entry->key = strdup(key);
    entry->value = value;
    entry->next = hashtable->entries[slot];
    hashtable->entries[slot] = entry;
}

static void ht_foreach(ht_t *hashtable) {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        ht_entry_t *entry = hashtable->entries[i];
        while (entry != NULL) {
            printf("%s|%d\n", entry->key, entry->value);
            entry = entry->next;
        }
    }
}

static void ht_destroy(ht_t *hashtable) {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        ht_entry_t *entry = hashtable->entries[i];
        while (entry != NULL) {
            ht_entry_t *next = entry->next;
            free(entry->key);
            free(entry);
            entry = next;
        }
    }
    free(hashtable->entries);
    free(hashtable);
}

// --- 查询工具主逻辑 ---

void print_usage(const char *prog_name) {
    fprintf(stderr, "用法: %s --dir <数据库目录> [选项]\n", prog_name);
    fprintf(stderr, "选项:\n");
    fprintf(stderr, "  --start-time <时间戳>  开始时间 (Unix秒)\n");
    fprintf(stderr, "  --end-time <时间戳>    结束时间 (Unix秒)\n");
    fprintf(stderr, "  --pid <pid>            按进程ID过滤\n");
    fprintf(stderr, "  --name <名称>          按进程名过滤 (精确匹配)\n");
}

int main(int argc, char *argv[]) {
    const char *db_dir = NULL;
    long long start_time = 0;
    long long end_time = 0;
    int pid = 0;
    const char *name = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            db_dir = argv[++i];
        } else if (strcmp(argv[i], "--start-time") == 0 && i + 1 < argc) {
            start_time = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--end-time") == 0 && i + 1 < argc) {
            end_time = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pid = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (!db_dir) {
        fprintf(stderr, "错误: --dir 参数是必需的。\n");
        print_usage(argv[0]);
        return 1;
    }
    
    if (end_time == 0) {
        end_time = time(NULL);
    }

    ht_t* aggregated_results = ht_create();

    time_t current_hour_ts = start_time - (start_time % 3600);
    while (current_hour_ts <= end_time) {
        struct tm tm;
        localtime_r(&current_hour_ts, &tm); // 使用 localtime_r 获取本地时间
        char db_path[256];
        snprintf(db_path, sizeof(db_path), "%s/profiling_%04d-%02d-%02d_%02d.db",
                 db_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour);
        fprintf(stderr, "DEBUG: Checking DB path: %s\n", db_path);

        if (access(db_path, F_OK) != 0) {
            fprintf(stderr, "DEBUG: DB file not found or inaccessible: %s, errno: %d\n", db_path, errno);
            current_hour_ts += 3600;
            continue;
        }

        sqlite3 *db;
        if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL)) {
            fprintf(stderr, "无法打开数据库: %s (%s)\n", db_path, sqlite3_errmsg(db));
            current_hour_ts += 3600;
            continue;
        }

        char sql[1024];
        char where_clauses[512] = " WHERE 1=1 ";
        int offset = strlen(where_clauses);

        if (start_time > 0) {
            fprintf(stderr, "DEBUG: Query start_time (ns): %lld\n", start_time * 1000000000LL);
            offset += snprintf(where_clauses + offset, sizeof(where_clauses) - offset, " AND timestamp >= %lld", start_time * 1000000000LL);
        }
        if (end_time > 0) {
            fprintf(stderr, "DEBUG: Query end_time (ns): %lld\n", end_time * 1000000000LL);
            offset += snprintf(where_clauses + offset, sizeof(where_clauses) - offset, " AND timestamp <= %lld", end_time * 1000000000LL);
        }
        if (pid > 0) {
            offset += snprintf(where_clauses + offset, sizeof(where_clauses) - offset, " AND pid = %d", pid);
        }
        if (name) {
            offset += snprintf(where_clauses + offset, sizeof(where_clauses) - offset, " AND process_name = '%s'", name);
        }

        snprintf(sql, sizeof(sql), "SELECT full_stack, COUNT(*) FROM call_stacks %s GROUP BY full_stack;", where_clauses);

        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
            fprintf(stderr, "无法准备SQL语句: %s\n", sqlite3_errmsg(db));
            sqlite3_close(db);
            current_hour_ts += 3600;
            continue;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *stack = sqlite3_column_text(stmt, 0);
            int count = sqlite3_column_int(stmt, 1);
            if (stack) {
                ht_insert(aggregated_results, (const char*)stack, count);
            }
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);
        current_hour_ts += 3600;
    }

    ht_foreach(aggregated_results);
    ht_destroy(aggregated_results);

    return 0;
}