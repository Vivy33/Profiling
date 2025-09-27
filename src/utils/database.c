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
#include "../../include/config.h"

// Latency histogram
#define NUM_LATENCY_BUCKETS 10
static long long latency_buckets[NUM_LATENCY_BUCKETS] = {0};
static long long push_count = 0;
static const long long bucket_thresholds[NUM_LATENCY_BUCKETS] = {
    1000,    // < 1 us
    5000,    // < 5 us
    10000,   // < 10 us
    50000,   // < 50 us
    100000,  // < 100 us
    500000,  // < 500 us
    1000000, // < 1 ms
    5000000, // < 5 ms
    10000000, // < 10 ms
    -1       // > 10 ms
};

/**
 * @file database.c
 * @brief 数据库写入模块的实现。
 *
 * 该模块负责将性能分析数据（调用栈）异步写入 SQLite 数据库。
 * 它使用一个并发队列来接收数据，并通过一个独立的线程进行批量写入，
 * 以提高写入性能并减少主线程的阻塞。
 * 数据库文件按小时轮换，并支持 WAL (Write-Ahead Logging) 模式以提高并发性。
 */

#define QUEUE_CAPACITY 1024 /**< 数据库写入队列的容量 */
/**
 * @brief 数据库条目结构体。
 *
 * 用于在队列中存储待写入数据库的单个性能数据。
 */
typedef struct {
    uint64_t timestamp_ns; /**< 采样时间戳 (纳秒) */
    char *stack_str;      /**< 完整的调用栈字符串 */
} db_entry_t;

/**
 * @brief 数据库写入器上下文结构体。
 *
 * 封装了数据库写入线程的所有状态和资源。
 */
struct db_writer_context_t {
    concurrent_queue_t *queue;      /**< 用于接收 db_entry_t 的并发队列 */
    char *output_dir;               /**< 数据库文件存储的输出目录 */
    pthread_t thread_id;            /**< 数据库写入线程的 ID */
    volatile bool running;          /**< 线程运行标志，控制线程生命周期 */
    struct sqlite3 *db;             /**< 当前打开的 SQLite 数据库句柄 */
    char current_db_path[256];      /**< 当前正在写入的数据库文件的路径 */
    const profiling_config_t *config; /**< Profiling configuration */
};

// 静态函数声明
static int ensure_db_handle(db_writer_context_t *context, uint64_t timestamp_ns);
static void *db_writer_thread_func(void *arg);

/**
 * @brief 初始化数据库写入器上下文。
 *
 * 创建输出目录，初始化并发队列，并分配 db_writer_context_t 结构体。
 *
 * @param output_dir 数据库文件将存储的目录路径。
 * @return 成功时返回 db_writer_context_t 指针，失败时返回 NULL。
 */
db_writer_context_t* db_writer_init(const profiling_config_t *config) {
    const char *output_dir = config->db_output_dir;
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
    context->config = config;

    return context;
}

/**
 * @brief 启动数据库写入线程。
 *
 * 创建一个独立的线程来执行 db_writer_thread_func。
 *
 * @param context 数据库写入器上下文指针。
 */
void db_writer_start(db_writer_context_t *context) {
    if (!context) return;
    context->running = true;
    if (pthread_create(&context->thread_id, NULL, db_writer_thread_func, context)) {
        perror("无法创建数据库写入线程");
        context->running = false;
    }
}

/**
 * @brief 停止数据库写入线程。
 *
 * 设置运行标志为 false，并向队列发送关闭信号，唤醒等待的线程。
 *
 * @param context 数据库写入器上下文指针。
 */
void db_writer_stop(db_writer_context_t *context) {
    if (!context) return;
    context->running = false;
    queue_signal_shutdown(context->queue);
}

/**
 * @brief 等待数据库写入线程完成并清理资源。
 *
 * 阻塞直到写入线程结束，然后关闭数据库句柄，释放所有动态分配的内存。
 *
 * @param context 数据库写入器上下文指针。
 */
void db_writer_wait(db_writer_context_t *context) {
    if (!context || context->thread_id == 0) return;
    pthread_join(context->thread_id, NULL);
    if (context->db) {
        sqlite3_close(context->db);
    }
    free(context->output_dir);
    // 清理队列中可能剩余的元素
    void* item;
    while((item = queue_pop(context->queue)) != NULL) {
        db_entry_t *entry = (db_entry_t *)item;
        free(entry->stack_str);
        free(entry);
    }
    queue_destroy(context->queue);
    free(context);
}

/**
 * @brief 将调用栈数据推送到数据库写入队列。
 *
 * 将时间戳和调用栈字符串封装成 db_entry_t 结构体，并推送到并发队列中。
 *
 * @param context 数据库写入器上下文指针。
 * @param timestamp_ns 采样时间戳 (纳秒)。
 * @param stack_str 完整的调用栈字符串。
 */
void db_writer_push_stack(db_writer_context_t *context, uint64_t timestamp_ns, const char *stack_str) {
    if (!context || !context->running) return;
    
    db_entry_t *entry = malloc(sizeof(db_entry_t));
    if (!entry) return;

    entry->stack_str = strdup(stack_str);
    if (!entry->stack_str) {
        free(entry);
        return;
    }
    
    // get start time
    entry->timestamp_ns = timestamp_ns;
    // fprintf(stderr, "DEBUG: db_writer_push_stack: Pushing timestamp_ns = %lu\n", timestamp_ns);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    // get end time
    // 如果时间过长说明出现了锁等待
    queue_push(context->queue, entry);

    clock_gettime(CLOCK_MONOTONIC, &end);
    long long elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
    
    int queue_size = queue_get_size(context->queue);
    fprintf(stderr, "DEBUG: queue_push latency: %lld ns, queue size: %d\n", elapsed_ns, queue_size);

    for (int i = 0; i < NUM_LATENCY_BUCKETS; ++i) {
        if (bucket_thresholds[i] == -1 || elapsed_ns < bucket_thresholds[i]) {
            latency_buckets[i]++;
            break;
        }
    }

    push_count++;
    if (push_count >= context->config->histogram_print_threshold) {
        fprintf(stderr, "DEBUG: Latency bucket distribution:\n");
        for (int i = 0; i < NUM_LATENCY_BUCKETS; ++i) {
            if (bucket_thresholds[i] == -1) {
                fprintf(stderr, "    > %lld ns: %lld\n", bucket_thresholds[i-1], latency_buckets[i]);
            } else {
                fprintf(stderr, "    < %lld ns: %lld\n", bucket_thresholds[i], latency_buckets[i]);
            }
        }
        memset(latency_buckets, 0, sizeof(latency_buckets));
        push_count = 0;
    }
}

/**
 * @brief 确保数据库句柄有效并指向正确的数据库文件。
 *
 * 根据给定的时间戳，确定当前应该写入的数据库文件路径。
 * 如果当前句柄无效或指向不同的文件，则关闭旧句柄，打开新文件，并创建表（如果不存在）。
 * 启用 WAL 模式以支持并发读写。
 *
 * @param context 数据库写入器上下文指针。
 * @param timestamp_ns 用于确定数据库文件的时间戳 (纳秒)。
 * @return 0 表示成功，-1 表示失败。
 */
static int ensure_db_handle(db_writer_context_t *context, uint64_t timestamp_ns) {
    long long tv_sec_ll = timestamp_ns / 1000000000LL; // 使用LL确保是long long除法
    time_t tv_sec = (time_t)tv_sec_ll;
    struct tm tm;
    localtime_r(&tv_sec, &tm); // 使用 localtime_r 获取本地时间

    char new_db_path[256];
    // 构建新的数据库文件路径，按小时命名
    snprintf(new_db_path, sizeof(new_db_path), "%s/profiling_%04d-%02d-%02d_%02d.db",
             context->output_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour);

    // 如果当前数据库路径与新路径相同，则直接返回
    if (strcmp(context->current_db_path, new_db_path) == 0) {
        return context->db != NULL ? 0 : -1;
    }

    // 如果存在旧的数据库句柄，则关闭它
    if (context->db) {
        sqlite3_close(context->db);
        context->db = NULL;
    }

    // 更新当前数据库路径
    snprintf(context->current_db_path, sizeof(context->current_db_path), "%s", new_db_path);

    // 打开新的数据库文件
    if (sqlite3_open(context->current_db_path, &context->db)) {
        fprintf(stderr, "DEBUG: 无法打开数据库: %s, 路径: %s\n", sqlite3_errmsg(context->db), context->current_db_path);
        context->db = NULL;
        return -1;
    }
    fprintf(stderr, "DEBUG: 数据库已打开: %s\n", context->current_db_path);

    // 启用 WAL (Write-Ahead Logging) 模式以支持并发访问
    char *err_msg = 0;
    if (sqlite3_exec(context->db, "PRAGMA journal_mode=WAL;", 0, 0, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "SQL 错误 (启用WAL失败): %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(context->db); // 在失败时关闭句柄
        context->db = NULL;
        return -1;
    }

    // 创建 call_stacks 表（如果不存在）
    const char *sql = "CREATE TABLE IF NOT EXISTS call_stacks (timestamp INTEGER, pid INTEGER, process_name TEXT, full_stack TEXT);";
    if (sqlite3_exec(context->db, sql, 0, 0, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "SQL 错误: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(context->db);
        context->db = NULL;
        return -1;
    }
    fprintf(stderr, "DEBUG: Table created or already exists.\n");
    return 0;
}

/**
 * @brief 数据库写入线程的主函数。
 *
 * 该线程从并发队列中批量弹出数据，并将其写入 SQLite 数据库。
 * 它会根据时间戳自动切换数据库文件，并使用事务来提高写入性能。
 *
 * @param arg 指向 db_writer_context_t 结构体的指针。
 * @return 总是返回 NULL。
 */
static void *db_writer_thread_func(void *arg) {
    db_writer_context_t *context = (db_writer_context_t *)arg;
    sqlite3_stmt *stmt = NULL;
    
    db_entry_t **batch = malloc(sizeof(db_entry_t*) * context->config->db_batch_size);
    if (!batch) {
        fprintf(stderr, "Failed to allocate memory for batch\n");
        return NULL;
    }

    int count = 0;
    fprintf(stderr, "DEBUG: DB writer thread started.\n");

    // 线程运行条件：context->running 为 true 或队列中仍有数据
    while (context->running || queue_get_size(context->queue) > 0) {
        // 尝试从队列中弹出单个元素
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
        // 批处理条件：达到 BATCH_SIZE 或线程即将停止 (context->running 为 false)
        if (count > 0 && (count >= context->config->db_batch_size || !context->running)) {
            fprintf(stderr, "DEBUG: Processing batch, count: %d\n", count);
            // 检查批次中的第一个元素是否有效，以避免潜在的空指针解引用
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
            // 确保数据库句柄有效并指向正确的文件
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

            // 准备 SQL 插入语句
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

            sqlite3_exec(context->db, "BEGIN TRANSACTION;", 0, 0, 0); // 开始事务
            
            for (int i = 0; i < count; i++) {
                db_entry_t *entry = batch[i];
                
                // 确保我们仍在使用正确的数据库文件
                // 如果时间戳跨越了小时边界，可能需要切换数据库文件
                if (ensure_db_handle(context, entry->timestamp_ns) != 0) {
                    // 如果DB句柄改变，我们需要重新开始事务
                    sqlite3_exec(context->db, "COMMIT;", 0, 0, 0); // 提交之前的事务
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
                
                // 解析调用栈字符串，格式为 "pid|process_name|full_stack"
                char *token = strtok_r(str_to_parse, "|", &saveptr);
                int pid = token ? atoi(token) : -1;
                token = strtok_r(NULL, "|", &saveptr);
                const char *process_name = token ? token : "unknown";
                const char *full_stack = strtok_r(NULL, "", &saveptr);
                if (!full_stack) full_stack = "";

                // 绑定参数到 SQL 语句（注意内存管理策略）：
                // - TEXT 参数统一使用 SQLITE_TRANSIENT，SQLite 会复制传入字符串，避免悬空指针。
                // - 之前使用 SQLITE_STATIC 在异步写入线程场景下可能造成悬空引用。
                sqlite3_bind_int64(stmt, 1, entry->timestamp_ns);
                sqlite3_bind_int(stmt, 2, pid);
                sqlite3_bind_text(stmt, 3, process_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, full_stack, -1, SQLITE_TRANSIENT);

                // 执行 SQL 插入步骤
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    fprintf(stderr, "DEBUG: 执行SQL插入步骤失败: %s\n", sqlite3_errmsg(context->db));
                } else {
                    fprintf(stderr, "DEBUG: SQL insert step successful.\n");
                }
                sqlite3_reset(stmt); // 重置语句以便下次使用

                free(entry->stack_str);
                free(entry);
            }
            
            if (count > 0) {
                sqlite3_exec(context->db, "COMMIT;", 0, 0, 0); // 提交事务
                fprintf(stderr, "DEBUG: Transaction committed.\n");
            }
            sqlite3_finalize(stmt); // 释放 SQL 语句句柄
            stmt = NULL;
            count = 0;
        }

        // 如果线程停止且队列为空，则退出循环
        if (!context->running && item == NULL) break;
    }
    fprintf(stderr, "DEBUG: DB writer thread stopped.\n");

    return NULL;
}

/**
 * @brief 获取当前数据库句柄。
 *
 * 此函数提供对当前 SQLite 数据库句柄的访问。
 * 注意：此函数没有加锁，依赖于 WAL 模式的并发安全性和句柄切换的低频率。
 *
 * @param context 数据库写入器上下文指针。
 * @return 当前的 SQLite 数据库句柄，如果上下文无效则返回 NULL。
 */
struct sqlite3* db_writer_get_db_handle(db_writer_context_t *context) {
    if (!context) {
        return NULL;
    }
    // 注意：这里没有加锁，因为我们依赖于
    // 1. WAL模式保证读写并发安全
    // 2. 句柄的切换只在写入线程中发生，且频率很低（每小时一次）
    // 3. 最坏情况下，查询可能会在一个刚关闭的句柄上失败，这是可以接受的
    return context->db;
}