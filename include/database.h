#ifndef DATABASE_H
#define DATABASE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"

// 前向声明 sqlite3 结构体，避免在头文件中引入整个 sqlite3.h
struct sqlite3;

typedef struct db_writer_context_t db_writer_context_t;

/**
 * @brief 初始化数据库写入器。
 *
 * 创建并初始化一个数据库写入器上下文。此函数会打开指定的SQLite数据库文件，
 * 创建必要的表结构，并初始化一个并发队列用于缓冲数据。
 *
 * @param db_path 要打开或创建的数据库文件的路径。
 * @return 成功时返回一个指向 db_writer_context_t 的指针，失败时返回 NULL。
 */
db_writer_context_t* db_writer_init(const char *db_path, const profiling_config_t *config);

/**
 * @brief 启动数据库后台写入线程。
 *
 * 创建并启动一个后台线程，该线程会持续从队列中读取数据并批量写入数据库。
 *
 * @param context 通过 db_writer_init 返回的上下文指针。
 */
void db_writer_start(db_writer_context_t *context);

/**
 * @brief 请求停止数据库后台写入线程。
 *
 * 向后台线程发送停止信号。线程会在处理完队列中剩余的所有数据后退出。
 * 这是一个非阻塞函数。
 *
 * @param context 上下文指针。
 */
void db_writer_stop(db_writer_context_t *context);

/**
 * @brief 等待数据库写入线程完全停止并清理资源。
 *
 * 阻塞当前线程，直到数据库后台写入线程完成所有工作并退出。
 * 之后，此函数会关闭数据库连接，销毁队列，并释放所有相关内存。
 *
 * @param context 上下文指针。
 */
void db_writer_wait(db_writer_context_t *context);

/**
 * @brief 向队列中推送一条调用栈数据。
 *
 * 此函数由主线程（或数据产生线程）调用，将格式化后的调用栈字符串推送到并发队列中，
 * 以便后台线程后续处理。该函数会复制传入的字符串。
 *
 * @param context 上下文指针。
 * @param timestamp_ns 内核提供的纳秒级时间戳
 * @param stack_str 要写入数据库的、格式化后的调用栈字符串。
 */
void db_writer_push_stack(db_writer_context_t *context, uint64_t timestamp_ns, const char *stack_str);

// 新增函数：获取数据库句柄，供HTTP服务器查询使用
struct sqlite3* db_writer_get_db_handle(db_writer_context_t *context);

#endif // DATABASE_H
