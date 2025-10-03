#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <errno.h>
#include "include/database.h"
#include "include/concurrent_queue.h"
#include "include/config.h"
#include "include/mempool.h"

// 推入操作的延迟直方图
#define NUM_LATENCY_BUCKETS 10
static long long push_latency_buckets[NUM_LATENCY_BUCKETS] = {0};
static long long push_count = 0;

// 弹出操作的延迟直方图
static long long pop_latency_buckets[NUM_LATENCY_BUCKETS] = {0};
static long long pop_count = 0;

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
    const struct profiling_config *config; /**< Profiling configuration */
    struct mempool_s *entry_pool;    /**< 内存池，用于分配 db_entry_t */
};

// 静态函数声明
static int ensure_db_handle(db_writer_context_t *context, uint64_t timestamp_ns);
static void *db_writer_thread_func(void *arg);

// 为生产者批处理定义静态缓冲区和计数器
static db_entry_t* producer_batch_buffer[MAX_PRODUCER_BATCH_SIZE];
static int producer_batch_count = 0;

/**
 * @brief 将生产者缓冲区中剩余的数据项刷入队列。
 *
 * 这是一个辅助函数，用于在缓冲区满或程序退出时，
 * 将缓冲区中所有待处理的数据项通过一次 queue_push_batch 调用推入队列。
 *
 * @param context 数据库写入器上下文指针。
 */
static void db_writer_flush(db_writer_context_t *context) {
    if (producer_batch_count > 0) {
        long long push_latency_ns = 0;
        int queue_size_before_push = queue_get_size(context->queue);
        // 时间过长说明出现锁等待
        queue_push_batch(context->queue, (void**)producer_batch_buffer, producer_batch_count, &push_latency_ns);
        producer_batch_count = 0;

        fprintf(stderr, "DEBUG: queue_push_batch latency: %lld ns, queue size before push: %d\n", push_latency_ns, queue_size_before_push);

        for (int i = 0; i < NUM_LATENCY_BUCKETS; ++i) {
            if (bucket_thresholds[i] == -1 || push_latency_ns < bucket_thresholds[i]) {
                push_latency_buckets[i]++;
                break;
            }
        }

        push_count++;
        if (push_count >= context->config->histogram_print_threshold) {
            FILE *log_file = fopen(context->config->histogram_log_path, "a");
            if (log_file) {
                time_t now;
                time(&now);
                char time_buf[32];
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));

                fprintf(log_file, "[%s] Queue push BATCH latency histogram (ns):\n", time_buf);
                for (int i = 0; i < NUM_LATENCY_BUCKETS; ++i) {
                    if (bucket_thresholds[i] == -1) {
                        fprintf(log_file, "    > %lld ns: %lld\n", bucket_thresholds[i-1], push_latency_buckets[i]);
                    } else {
                        fprintf(log_file, "    < %lld ns: %lld\n", bucket_thresholds[i], push_latency_buckets[i]);
                    }
                }
                fprintf(log_file, "\n");
                fclose(log_file);
                fprintf(stderr, "DEBUG: Histogram written to %s\n", context->config->histogram_log_path);
            } else {
                fprintf(stderr, "ERROR: Could not open histogram log file %s: %s\n", context->config->histogram_log_path, strerror(errno));
            }

            memset(push_latency_buckets, 0, sizeof(push_latency_buckets));
            push_count = 0;
        }
    }
}

/**
 * @brief 初始化数据库写入器上下文。
 *
 * 创建输出目录，初始化并发队列，并分配 db_writer_context_t 结构体。
 *
 * @param output_dir 数据库文件将存储的目录路径。
 * @return 成功时返回 db_writer_context_t 指针，失败时返回 NULL。
 */
db_writer_context_t* db_writer_init(const struct profiling_config *config) {
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

    // 初始化内存池
    // 每个条目的大小需要足够容纳 db_entry_t 结构体以及最长的可能调用栈字符串。
    // 一个符号最长按256字节计算，加上分隔符，总长度为 max_stack_depth * 257
    size_t max_stack_str_len = config->max_stack_depth * 257;
    size_t entry_size = sizeof(db_entry_t) + max_stack_str_len;

    // 详细计算 entry_size:
    // - sizeof(db_entry_t) 通常是 16 字节 (取决于具体的结构体定义和对齐方式)。
    // - config->max_stack_depth 的默认值是 48 (DEFAULT_MAX_STACK_DEPTH)。
    // - max_stack_str_len = 48 * 257 = 12336 字节。
    // - entry_size = 16 + 12336 = 12352 字节，约等于 12KB。
    //
    // 内存池总大小计算:
    // - config->db_entry_pool_size 的默认值是 8192 (DEFAULT_DB_ENTRY_POOL_SIZE)。
    // - 总大小 = 8192 * 12352 字节 = 101,185,536 字节 ≈ 96.5MB。
    context->entry_pool = mempool_create(config->db_entry_pool_size, entry_size);
    if (!context->entry_pool) {
        fprintf(stderr, "无法创建数据库条目内存池\n");
        queue_destroy(context->queue);
        free(context->output_dir);
        free(context);
        return NULL;
    }

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
    // 刷入所有剩余的条目
    db_writer_flush(context);
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
        // 由于使用了内存池，这里不再需要手动释放
        // free(((db_entry_t *)item)->stack_str);
        // free(item);
    }
    queue_destroy(context->queue);
    mempool_destroy(context->entry_pool); // 销毁内存池
    free(context);
}

/**
 * @brief 将一批数据条目安全地推入数据库写入器的队列中。
 *
 * 将时间戳和调用栈字符串封装成 db_entry_t 结构体，并推送到并发队列中。
 *
 * @param context 数据库写入器上下文指针。
 * @param timestamp_ns 采样时间戳 (纳秒)。
 * @param stack_str 完整的调用栈字符串。
 */
void db_writer_push_stack(db_writer_context_t *context, uint64_t timestamp_ns, const char *stack_str) {
    if (!context || !context->running) return;

    size_t stack_len = strlen(stack_str);
    // 从内存池分配一个能容纳 db_entry_t 和 stack_str 的连续块
    db_entry_t *entry = (db_entry_t *)mempool_alloc(context->entry_pool);
    if (!entry) {
        fprintf(stderr, "从内存池分配失败\n");
        return;
    }
    
    entry->timestamp_ns = timestamp_ns;
    // 将字符串数据紧随 db_entry_t 之后存储
    entry->stack_str = (char *)(entry + 1);
    memcpy(entry->stack_str, stack_str, stack_len + 1);

    // 将条目添加到生产者缓冲区
    producer_batch_buffer[producer_batch_count++] = entry;

    // 根据配置的生产者批处理阈值进行刷入；同时遵守物理上限以避免越界
    int flush_threshold = context->config->producer_batch_size;
    if (flush_threshold <= 0 || flush_threshold > MAX_PRODUCER_BATCH_SIZE) {
        flush_threshold = MAX_PRODUCER_BATCH_SIZE;
    }
    if (producer_batch_count > 0 && producer_batch_count >= flush_threshold) {
        db_writer_flush(context);
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

    // 为 timestamp 创建索引
    const char *sql_index = "CREATE INDEX IF NOT EXISTS idx_timestamp ON call_stacks (timestamp);";
    if (sqlite3_exec(context->db, sql_index, 0, 0, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "SQL 错误 (创建索引失败): %s\n", err_msg);
        sqlite3_free(err_msg);
    }
    fprintf(stderr, "DEBUG: Table created or already exists.\n");
    return 0;
}

/**
 * @brief 数据库写入线程的主函数。
 *
 * 该线程从并发队列中批量弹出数据，并将其写入 SQLite 数据库。
 * 它会根据时间戳自动切换数据库文件，并使用事务来提高写入性能。
 * 通过调用 queue_pop_batch 实现高效的批量消费，减少锁争用。
 *
 * @param arg 指向 db_writer_context_t 结构体的指针。
 * @return 总是返回 NULL。
 */
static void *db_writer_thread_func(void *arg) {
    db_writer_context_t *context = (db_writer_context_t *)arg;
    sqlite3_stmt *stmt = NULL;

    // 从配置中获取批处理大小
    int batch_size = context->config->db_batch_size;
    // 为批量处理分配内存缓冲区
    db_entry_t **batch = malloc(sizeof(db_entry_t*) * batch_size);
    if (!batch) {
        fprintf(stderr, "Failed to allocate memory for batch\n");
        return NULL;
    }

    fprintf(stderr, "DEBUG: DB writer thread started.\n");

    // 线程主循环：只要线程在运行或队列中还有数据，就继续处理
    while (context->running || queue_get_size(context->queue) > 0) {
        // 从队列中批量弹出一批数据项
        // 这是一个阻塞操作，直到有数据或队列关闭
        long long pop_latency_ns = 0;
        int count = queue_pop_batch(context->queue, (void**)batch, batch_size, &pop_latency_ns);

        if (count > 0) {
            fprintf(stderr, "DEBUG: queue_pop_batch latency: %lld ns, count: %d\n", pop_latency_ns, count);
        }

        // 更新 pop 延迟直方图
        for (int i = 0; i < NUM_LATENCY_BUCKETS; ++i) {
            if (bucket_thresholds[i] == -1 || pop_latency_ns < bucket_thresholds[i]) {
                pop_latency_buckets[i]++;
                break;
            }
        }
        pop_count++;

        // 如果没有弹出任何数据
        if (count == 0) {
            // 如果线程已停止，说明队列已空且不会再有新数据，可以安全退出
            if (!context->running) {
                fprintf(stderr, "DEBUG: Queue is empty and shutdown, exiting thread.\n");
                break;
            }
            // 如果线程仍在运行，但队列暂时为空，则继续下一次循环等待
            continue;
        }

        // 如果达到阈值，则打印 pop 延迟直方图
        if (pop_count >= context->config->histogram_print_threshold) {
            FILE *log_file = fopen(context->config->histogram_log_path, "a");
            if (log_file) {
                time_t now;
                time(&now);
                char time_buf[32];
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));

                fprintf(log_file, "[%s] Queue pop BATCH latency histogram (ns):\n", time_buf);
                for (int i = 0; i < NUM_LATENCY_BUCKETS; ++i) {
                    if (bucket_thresholds[i] == -1) {
                        fprintf(log_file, "    > %lld ns: %lld\n", bucket_thresholds[i-1], pop_latency_buckets[i]);
                    } else {
                        fprintf(log_file, "    < %lld ns: %lld\n", bucket_thresholds[i], pop_latency_buckets[i]);
                    }
                }
                fprintf(log_file, "\n");
                fclose(log_file);
            }
            memset(pop_latency_buckets, 0, sizeof(pop_latency_buckets));
            pop_count = 0;
        }

        // 使用批次中的第一个数据项的时间戳来确保数据库句柄有效
        // 这会处理按小时轮换数据库文件的逻辑
        if (ensure_db_handle(context, batch[0]->timestamp_ns) != 0) {
            fprintf(stderr, "DEBUG: Failed to get DB handle, discarding batch.\n");
            // 如果无法获取数据库句柄，则丢弃整个批次以避免阻塞
            for (int i = 0; i < count; i++) {
                free(batch[i]->stack_str);
                free(batch[i]);
            }
            continue;
        }

        // 准备 SQL 插入语句
        const char *sql = "INSERT INTO call_stacks (timestamp, pid, process_name, full_stack) VALUES (?, ?, ?, ?);";
        if (sqlite3_prepare_v2(context->db, sql, -1, &stmt, 0) != SQLITE_OK) {
            fprintf(stderr, "无法准备SQL语句: %s\n", sqlite3_errmsg(context->db));
            // 如果准备失败，丢弃批次
            for (int i = 0; i < count; i++) {
                free(batch[i]->stack_str);
                free(batch[i]);
            }
            continue;
        }

        // 开启数据库事务，以大幅提高批量插入的性能
        sqlite3_exec(context->db, "BEGIN TRANSACTION;", 0, 0, 0);

        // 遍历批次中的每一个数据项
        for (int i = 0; i < count; i++) {
            db_entry_t *entry = batch[i];

            // 再次检查数据库句柄，以处理批次内时间戳跨越小时边界的情况
            if (ensure_db_handle(context, entry->timestamp_ns) != 0) {
                // 如果句柄发生变化，提交当前事务并丢弃批次中剩余的数据项
                sqlite3_exec(context->db, "COMMIT;", 0, 0, 0);
                for (int j = i; j < count; j++) {
                    free(batch[j]->stack_str);
                    free(batch[j]);
                }
                count = 0; // 标记批次已被丢弃
                break;
            }

            // 解析调用栈字符串，格式为 "pid|process_name|full_stack"
            char *str_to_parse = entry->stack_str;
            char *saveptr;
            char *token = strtok_r(str_to_parse, "|", &saveptr);
            int pid = token ? atoi(token) : -1;
            token = strtok_r(NULL, "|", &saveptr);
            const char *process_name = token ? token : "unknown";
            const char *full_stack = strtok_r(NULL, "", &saveptr);
            if (!full_stack) full_stack = "";

            // 将解析出的数据绑定到预准备的 SQL 语句
            sqlite3_bind_int64(stmt, 1, entry->timestamp_ns);
            sqlite3_bind_int(stmt, 2, pid);
            sqlite3_bind_text(stmt, 3, process_name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, full_stack, -1, SQLITE_TRANSIENT);

            // 执行单次插入
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                fprintf(stderr, "DEBUG: 执行SQL插入步骤失败: %s\n", sqlite3_errmsg(context->db));
            }
            // 重置语句以便下一次循环使用
            sqlite3_reset(stmt);

            // 内存将由批处理循环结束后的 mempool_free 统一释放
        }

        // 如果批次被成功处理（或部分处理），提交事务
        if (count > 0) {
            sqlite3_exec(context->db, "COMMIT;", 0, 0, 0);
        }
        // 释放 SQL 语句句柄
        sqlite3_finalize(stmt);

        // 释放从队列中取出的所有条目
        for (int i = 0; i < count; ++i) {
            mempool_free(context->entry_pool, batch[i]);
        }
    }

    // 释放为批处理分配的内存缓冲区
    free(batch);
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