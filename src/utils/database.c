#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <errno.h>
#include "../../include/database.h"
#include "../../include/concurrent_queue.h"

#define QUEUE_CAPACITY 1024
#define BATCH_SIZE 100

typedef struct {
    uint64_t timestamp_ns;
    char *stack_str;
} db_entry_t;

struct db_writer_context_t {
    concurrent_queue_t *queue;
    char *output_dir;
    pthread_t thread_id;
    volatile bool running;
    sqlite3 *db;
    char current_db_path[256];
};

static int ensure_db_handle(db_writer_context_t *context, uint64_t timestamp_ns);
static void *db_writer_thread_func(void *arg);

db_writer_context_t* db_writer_init(const char *output_dir) {
    // 检查并创建输出目录
    struct stat st = {0};
    if (stat(output_dir, &st) == -1) {
        if (mkdir(output_dir, 0755) != 0) {
            perror("无法创建数据库输出目录");
            fprintf(stderr, "DEBUG: Failed to create output directory: %s, errno: %d\n", output_dir, errno);
            return NULL;
        }
        fprintf(stderr, "DEBUG: Output directory created: %s\n", output_dir);
    } else {
        fprintf(stderr, "DEBUG: Output directory already exists: %s\n", output_dir);
    }

    db_writer_context_t *context = malloc(sizeof(db_writer_context_t));
    if (!context) {
        perror("无法为 db_writer_context 分配内存");
        return NULL;
    }

    context->queue = queue_init(QUEUE_CAPACITY);
    if (!context->queue) {
        fprintf(stderr, "无法初始化并发队列\n");
        free(context);
        return NULL;
    }

    context->output_dir = strdup(output_dir);
    context->running = false;
    context->thread_id = 0;
    context->db = NULL;
    context->current_db_path[0] = '\0';

    return context;
}

void db_writer_start(db_writer_context_t *context) {
    if (!context) return;
    context->running = true;
    if (pthread_create(&context->thread_id, NULL, db_writer_thread_func, context)) {
        perror("无法创建数据库写入线程");
        context->running = false;
    }
}

void db_writer_stop(db_writer_context_t *context) {
    if (!context) return;
    context->running = false;
    queue_signal_shutdown(context->queue);
}

void db_writer_wait(db_writer_context_t *context) {
    if (!context || context->thread_id == 0) return;
    pthread_join(context->thread_id, NULL);
    if (context->db) {
        sqlite3_close(context->db);
    }
    free(context->output_dir);
    void* item;
    while((item = queue_pop(context->queue)) != NULL) {
        db_entry_t *entry = (db_entry_t *)item;
        free(entry->stack_str);
        free(entry);
    }
    queue_destroy(context->queue);
    free(context);
}

void db_writer_push_stack(db_writer_context_t *context, uint64_t timestamp_ns, const char *stack_str) {
    if (!context || !context->running) return;
    
    db_entry_t *entry = malloc(sizeof(db_entry_t));
    if (!entry) return;

    entry->stack_str = strdup(stack_str);
    if (!entry->stack_str) {
        free(entry);
        return;
    }
    
    entry->timestamp_ns = timestamp_ns;
    queue_push(context->queue, entry);
}

static int ensure_db_handle(db_writer_context_t *context, uint64_t timestamp_ns) {
    long long tv_sec_ll = timestamp_ns / 1000000000LL; // 使用LL确保是long long除法
    time_t tv_sec = (time_t)tv_sec_ll;
    struct tm tm;
    localtime_r(&tv_sec, &tm); // 使用 localtime_r 获取本地时间

    char new_db_path[256];
    snprintf(new_db_path, sizeof(new_db_path), "%s/profiling_%04d-%02d-%02d_%02d.db",
             context->output_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour);

    if (strcmp(context->current_db_path, new_db_path) == 0) {
        return context->db != NULL ? 0 : -1;
    }

    if (context->db) {
        sqlite3_close(context->db);
        context->db = NULL;
    }

    snprintf(context->current_db_path, sizeof(context->current_db_path), "%s", new_db_path);

    if (sqlite3_open(context->current_db_path, &context->db)) {
        fprintf(stderr, "DEBUG: 无法打开数据库: %s, 路径: %s\n", sqlite3_errmsg(context->db), context->current_db_path);
        context->db = NULL;
        return -1;
    }
    fprintf(stderr, "DEBUG: 数据库已打开: %s\n", context->current_db_path);

    char *err_msg = 0;
    if (sqlite3_exec(context->db, "PRAGMA journal_mode=WAL;", 0, 0, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "SQL 错误 (启用WAL失败): %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    const char *sql = "CREATE TABLE IF NOT EXISTS call_stacks ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "timestamp INTEGER NOT NULL,"
                      "pid INTEGER NOT NULL,"
                      "process_name TEXT,"
                      "full_stack TEXT NOT NULL);";
    if (sqlite3_exec(context->db, sql, 0, 0, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "SQL 错误: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(context->db);
        context->db = NULL;
        return -1;
    }
    return 0;
}

static void *db_writer_thread_func(void *arg) {
    db_writer_context_t *context = (db_writer_context_t *)arg;
    sqlite3_stmt *stmt = NULL;
    
    db_entry_t *batch[BATCH_SIZE];
    int count = 0;
    fprintf(stderr, "DEBUG: DB writer thread started.\n");

    while (context->running || queue_size(context->queue) > 0) {
        void* item = queue_pop(context->queue);
        if (item) {
            batch[count++] = (db_entry_t*)item;
            fprintf(stderr, "DEBUG: Item popped from queue, count: %d\n", count);
        } else {
            // 如果队列已关闭且为空，queue_pop 会返回 NULL
            if (!context->running && count == 0) {
                fprintf(stderr, "DEBUG: Queue is empty and shutdown, exiting thread.\n");
                break;
            }
        }

        // 无论 item 是否为 NULL，只要有数据在 batch 中，并且满足批处理条件，就尝试处理
        if (count > 0 && (count >= BATCH_SIZE || !context->running)) {
            fprintf(stderr, "DEBUG: Processing batch, count: %d\n", count);
            if (batch[0] == NULL) {
                fprintf(stderr, "DEBUG: batch[0] is NULL, skipping ensure_db_handle.\n");
                // 丢弃批次以避免阻塞
                for (int i = 0; i < count; i++) {
                    if (batch[i]) {
                        free(batch[i]->stack_str);
                        free(batch[i]);
                    }
                }
                count = 0;
                continue;
            }
            fprintf(stderr, "DEBUG: Calling ensure_db_handle.\n");
            if (ensure_db_handle(context, batch[0]->timestamp_ns) != 0) {
                fprintf(stderr, "DEBUG: Failed to get DB handle, discarding batch.\n");
                // 无法获取DB句柄，丢弃批次以避免阻塞
                for (int i = 0; i < count; i++) {
                    free(batch[i]->stack_str);
                    free(batch[i]);
                }
                count = 0;
                continue;
            }

            const char *sql = "INSERT INTO call_stacks (timestamp, pid, process_name, full_stack) VALUES (?, ?, ?, ?);";
            if (sqlite3_prepare_v2(context->db, sql, -1, &stmt, 0) != SQLITE_OK) {
                fprintf(stderr, "无法准备SQL语句: %s\n", sqlite3_errmsg(context->db));
                // 丢弃批次
                for (int i = 0; i < count; i++) {
                    free(batch[i]->stack_str);
                    free(batch[i]);
                }
                count = 0;
                continue;
            }

            sqlite3_exec(context->db, "BEGIN TRANSACTION;", 0, 0, 0);
            
            for (int i = 0; i < count; i++) {
                db_entry_t *entry = batch[i];
                
                // 确保我们仍在使用正确的数据库文件
                if (ensure_db_handle(context, entry->timestamp_ns) != 0) {
                    // 如果DB句柄改变，我们需要重新开始事务
                    sqlite3_exec(context->db, "COMMIT;", 0, 0, 0); // 提交之前的
                    // 此处简化处理：丢弃剩余部分并重新开始循环
                    for (int j = i; j < count; j++) {
                        free(batch[j]->stack_str);
                        free(batch[j]);
                    }
                    count = 0;
                    break; 
                }

                char *str_to_parse = entry->stack_str;
                char *saveptr;
                
                char *token = strtok_r(str_to_parse, "|", &saveptr);
                int pid = token ? atoi(token) : -1;
                token = strtok_r(NULL, "|", &saveptr);
                const char *process_name = token ? token : "unknown";
                const char *full_stack = strtok_r(NULL, "", &saveptr);
                if (!full_stack) full_stack = "";

                sqlite3_bind_int64(stmt, 1, entry->timestamp_ns);
                sqlite3_bind_int(stmt, 2, pid);
                sqlite3_bind_text(stmt, 3, process_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 4, full_stack, -1, SQLITE_TRANSIENT);

                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    fprintf(stderr, "DEBUG: 执行SQL插入步骤失败: %s\n", sqlite3_errmsg(context->db));
                } else {
                    fprintf(stderr, "DEBUG: SQL insert step successful.\n");
                }
                sqlite3_reset(stmt);

                free(entry->stack_str);
                free(entry);
            }
            
            if (count > 0) {
                sqlite3_exec(context->db, "COMMIT;", 0, 0, 0);
                fprintf(stderr, "DEBUG: Transaction committed.\n");
            }
            sqlite3_finalize(stmt);
            stmt = NULL;
            count = 0;
        }

        if (!context->running && item == NULL) break;
    }
    fprintf(stderr, "DEBUG: DB writer thread stopped.\n");

    return NULL;
}
