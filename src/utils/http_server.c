#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <cjson/cJSON.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "include/http_server.h"
#include "include/database.h" // 包含 db_writer_get_db_handle

// 说明：为了解决缺少网络相关符号（sockaddr_in/htons/htonl/INADDR_LOOPBACK）导致的编译错误，
// 我们显式引入 sys/socket.h、netinet/in.h、arpa/inet.h 头文件；
// 另外引入 time.h 以便在默认结束时间中使用 time(NULL)。这样可以保证在仅绑定到本地回环地址时，
// 相关网络 API 都能正确解析和编译。

/**
 * @file http_server.c
 * @brief HTTP 服务器模块的实现。
 *
 * 该模块使用 libmicrohttpd 库实现一个简单的 HTTP 服务器，
 * 用于接收实时查询请求，并从 SQLite 数据库中检索性能分析数据。
 * 查询结果以 JSON 格式返回。
 */

// --- Helper Functions ---

/**
 * @brief 从 HTTP 连接中获取指定查询参数的值。
 *
 * @param connection MHD 连接句柄。
 * @param key 查询参数的键名。
 * @return 查询参数的值字符串，如果不存在则返回 NULL。
 */
static const char* get_query_param(struct MHD_Connection *connection, const char *key) {
    return MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, key);
}

// --- Request Handler ---

/**
 * @brief HTTP 请求处理回调函数。
 *
 * 这是 libmicrohttpd 的核心回调函数，用于处理所有传入的 HTTP 请求。
 * 它解析请求 URL 和参数，构建 SQL 查询，执行数据库操作，并返回 JSON 格式的响应。
 *
 * @param cls 用户自定义数据指针，此处为 http_server_context_t 实例。
 * @param connection MHD 连接句柄。
 * @param url 请求的 URL 路径。
 * @param method 请求的 HTTP 方法 (例如 "GET", "POST")。
 * @param version 请求的 HTTP 版本。
 * @param upload_data 上传数据 (对于 GET 请求通常为 NULL)。
 * @param upload_data_size 上传数据的大小。
 * @param con_cls 连接的私有数据指针。
 * @return MHD_YES 表示成功处理请求，MHD_NO 表示失败。
 */
static enum MHD_Result request_handler(void *cls, struct MHD_Connection *connection,
                           const char *url, const char *method,
                           const char *version, const char *upload_data, 
                           size_t *upload_data_size, void **con_cls) {
    
    http_server_context_t *context = (http_server_context_t *)cls;
    struct MHD_Response *response;
    enum MHD_Result ret;

    // 检查请求方法和URL，只处理 GET /query 请求
    if (strcmp(method, "GET") != 0 || strcmp(url, "/query") != 0) {
        const char *page = "{\"error\": \"Not Found\"}";
        response = MHD_create_response_from_buffer(strlen(page), (void *)page, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json");
        ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
        MHD_destroy_response(response);
        return ret;
    }

    // 1. 解析查询参数
    const char *start_time_str = get_query_param(connection, "start_time");
    const char *end_time_str = get_query_param(connection, "end_time");
    const char *pid_str = get_query_param(connection, "pid");
    const char *name_str = get_query_param(connection, "name");

    long long start_time = start_time_str ? atoll(start_time_str) : 0;
    long long end_time = end_time_str ? atoll(end_time_str) : time(NULL); // 默认结束时间为当前时间

    // 2. 构建动态SQL查询的 WHERE 子句
    char sql[1024];
    char where_clauses[512] = " WHERE 1=1 "; // 初始条件，方便后续追加 AND
    
    if (start_time > 0) strcat(where_clauses, " AND timestamp >= :start_time");
    if (end_time > 0) strcat(where_clauses, " AND timestamp <= :end_time");
    if (pid_str) strcat(where_clauses, " AND pid = :pid");
    if (name_str) strcat(where_clauses, " AND process_name = :name");

    // 构建完整的 SQL 查询语句
    snprintf(sql, sizeof(sql), "SELECT full_stack, COUNT(*) as count FROM call_stacks %s GROUP BY full_stack ORDER BY count DESC;", where_clauses);

    // 3. 执行查询
    // 获取数据库写入器提供的当前数据库句柄
    struct sqlite3 *db = db_writer_get_db_handle(context->db_context);
    if (!db) {
        const char *page = "{\"error\": \"Database not available\"}";
        response = MHD_create_response_from_buffer(strlen(page), (void *)page, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json");
        ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return ret;
    }

    sqlite3_stmt *stmt;
    cJSON *json_array = cJSON_CreateArray(); // 创建 JSON 数组来存储查询结果
    // 准备 SQL 语句
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        // 绑定参数到 SQL 语句
        if (start_time > 0) sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":start_time"), start_time * 1000000000LL); // 转换为纳秒
        if (end_time > 0) sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":end_time"), end_time * 1000000000LL);     // 转换为纳秒
        if (pid_str) sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":pid"), atoi(pid_str));
        if (name_str) sqlite3_bind_text(stmt, sqlite3_bind_parameter_index(stmt, ":name"), name_str, -1, SQLITE_STATIC);

        // 遍历查询结果集
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *stack = sqlite3_column_text(stmt, 0); // 获取调用栈
            int count = sqlite3_column_int(stmt, 1);                 // 获取计数
            
            cJSON *json_obj = cJSON_CreateObject(); // 为每个结果创建一个 JSON 对象
            cJSON_AddStringToObject(json_obj, "stack", (const char *)stack);
            cJSON_AddNumberToObject(json_obj, "count", count);
            cJSON_AddItemToArray(json_array, json_obj); // 将 JSON 对象添加到数组
        }
        sqlite3_finalize(stmt); // 释放 SQL 语句句柄
    } else {
        fprintf(stderr, "HTTP Server SQL Error: %s\n", sqlite3_errmsg(db));
    }

    char *json_string = cJSON_PrintUnformatted(json_array); // 将 JSON 数组转换为字符串
    cJSON_Delete(json_array);                                // 释放 JSON 数组对象

    // 4. 发送JSON响应
    response = MHD_create_response_from_buffer(strlen(json_string), json_string, MHD_RESPMEM_MUST_FREE); // 创建响应缓冲区
    MHD_add_response_header(response, "Content-Type", "application/json"); // 设置 Content-Type 头
    ret = MHD_queue_response(connection, MHD_HTTP_OK, response);           // 将响应排队发送
    MHD_destroy_response(response);                                        // 销毁响应对象

    return ret;
}

/**
 * @brief 启动 HTTP 服务器。
 *
 * 初始化 libmicrohttpd 守护进程，并设置请求处理回调函数。
 *
 * @param port 服务器监听的端口号。
 * @param db_context 数据库写入器上下文指针，用于访问数据库句柄。
 * @return 成功时返回 http_server_context_t 指针，失败时返回 NULL。
 */
http_server_context_t* http_server_start(int port, db_writer_context_t *db_context) {
    http_server_context_t *context = malloc(sizeof(http_server_context_t));
    if (!context) {
        fprintf(stderr, "Error: Failed to allocate memory for HTTP server context\n");
        return NULL;
    }

    context->db_context = db_context;
    // 仅绑定到本地回环地址，提升安全性：通过设置 MHD_OPTION_SOCK_ADDR 为 127.0.0.1，
    // 避免服务对外网暴露，默认只允许本机访问。若未来需要外部访问，可添加配置开关以切换绑定地址。
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    context->daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, port, NULL, NULL,
                                       &request_handler, context,
                                       MHD_OPTION_SOCK_ADDR, &addr,
                                       MHD_OPTION_END);

    if (NULL == context->daemon) {
        fprintf(stderr, "Error: Failed to start HTTP server on port %d\n", port);
        free(context);
        return NULL;
    }

    printf("HTTP server started on port %d (loopback only)\n", port);
    return context;
}

/**
 * @brief 停止 HTTP 服务器。
 *
 * 停止 libmicrohttpd 守护进程并释放相关资源。
 *
 * @param context HTTP 服务器上下文指针。
 */
void http_server_stop(http_server_context_t *context) {
    if (context && context->daemon) {
        MHD_stop_daemon(context->daemon);
        free(context);
        printf("HTTP server stopped.\n");
    }
}