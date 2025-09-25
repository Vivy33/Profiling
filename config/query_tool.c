#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

/**
 * @file query_tool.c
 * @brief 性能分析数据的查询工具。
 *
 * 该工具支持两种查询模式：
 * 1. 历史查询：从本地 SQLite 数据库文件中检索历史性能数据。
 * 2. 实时查询：通过 HTTP 请求从正在运行的性能分析服务器获取实时数据。
 *
 * 它还包含一个简单的哈希表实现，用于聚合查询结果。
 */

// --- 内存缓冲区，用于存储curl的响应 ---
/**
 * @brief 用于存储 cURL 响应数据的内存结构体。
 *
 * cURL 回调函数会将接收到的数据写入此结构体中的 memory 缓冲区。
 */
typedef struct {
    char *memory; /**< 动态分配的内存指针，用于存储响应数据 */
    size_t size;   /**< 当前存储的数据大小 */
} MemoryStruct;

/**
 * @brief cURL 的写入回调函数。
 *
 * 当 cURL 接收到数据时，会调用此函数。它将接收到的数据追加到 MemoryStruct 中。
 *
 * @param contents 指向接收到的数据的指针。
 * @param size 单个数据块的大小。
 * @param nmemb 数据块的数量。
 * @param userp 用户自定义数据指针，此处为 MemoryStruct 实例。
 * @return 实际处理的字节数。
 */
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;
    // 重新分配内存以容纳新数据和空终止符
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        fprintf(stderr, "内存不足 (realloc 返回 NULL)\n");
        return 0;
    }
    mem->memory = ptr;
    // 将新数据拷贝到缓冲区末尾
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0; // 添加空终止符
    return realsize;
}

// --- 内置哈希表实现 (公共) ---
#define HASH_TABLE_SIZE 4096 /**< 哈希表的大小 */

/**
 * @brief 哈希表条目结构体。
 *
 * 用于存储键值对，并支持链式冲突解决。
 */
typedef struct ht_entry {
    char *key;           /**< 键（字符串） */
    int value;           /**< 值（整数） */
    struct ht_entry *next; /**< 指向下一个条目的指针，用于解决冲突 */
} ht_entry_t;

/**
 * @brief 哈希表结构体。
 *
 * 包含一个指向条目数组的指针。
 */
typedef struct {
    ht_entry_t **entries; /**< 哈希表条目数组 */
} ht_t;

/**
 * @brief 计算给定字符串的哈希值。
 *
 * 使用 DJB2 哈希算法。
 *
 * @param key 要计算哈希值的字符串。
 * @return 哈希值。
 */
static unsigned int hash(const char *key) {
    unsigned long int hash = 5381;
    int c;
    while ((c = *key++))
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    return hash % HASH_TABLE_SIZE;
}

/**
 * @brief 创建并初始化一个新的哈希表。
 *
 * @return 指向新创建的哈希表的指针。
 */
static ht_t* ht_create(void) {
    ht_t *hashtable = malloc(sizeof(ht_t));
    if (!hashtable) return NULL;
    hashtable->entries = calloc(HASH_TABLE_SIZE, sizeof(ht_entry_t*));
    if (!hashtable->entries) {
        free(hashtable);
        return NULL;
    }
    return hashtable;
}

/**
 * @brief 向哈希表中插入一个键值对。
 *
 * 如果键已存在，则将新值累加到现有值上。
 *
 * @param hashtable 哈希表的指针。
 * @param key 要插入的键。
 * @param value 要插入的值。
 */
static void ht_insert(ht_t *hashtable, const char *key, int value) {
    unsigned int slot = hash(key);
    ht_entry_t *entry = hashtable->entries[slot];
    // 检查键是否已存在
    while (entry != NULL) {
        if (strcmp(entry->key, key) == 0) {
            entry->value += value;
            return;
        }
        entry = entry->next;
    }
    // 键不存在，创建新条目
    entry = malloc(sizeof(ht_entry_t));
    if (!entry) return;
    entry->key = strdup(key);
    if (!entry->key) {
        free(entry);
        return;
    }
    entry->value = value;
    entry->next = hashtable->entries[slot];
    hashtable->entries[slot] = entry;
}

/**
 * @brief 遍历哈希表并打印所有键值对。
 *
 * 格式为 "key|value"。
 *
 * @param hashtable 哈希表的指针。
 */
static void ht_foreach(ht_t *hashtable) {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        ht_entry_t *entry = hashtable->entries[i];
        while (entry != NULL) {
            printf("%s|%d\n", entry->key, entry->value);
            entry = entry->next;
        }
    }
}

/**
 * @brief 销毁哈希表并释放所有相关内存。
 *
 * @param hashtable 要销毁的哈希表的指针。
 */
static void ht_destroy(ht_t *hashtable) {
    if (!hashtable) return;
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

// --- 实时查询 (HTTP模式) ---
/**
 * @brief 执行实时性能数据查询。
 *
 * 通过 HTTP GET 请求向指定的服务器发送查询，并解析 JSON 响应。
 *
 * @param start_time 查询的开始时间戳 (Unix秒)。
 * @param end_time 查询的结束时间戳 (Unix秒)。
 * @param pid 要过滤的进程ID (0 表示不过滤)。
 * @param name 要过滤的进程名称 (NULL 表示不过滤)。
 * @param server_url 性能分析服务器的 URL。
 * @return 0 表示成功，非 0 表示失败。
 */
static int perform_live_query(long long start_time, long long end_time, int pid, const char *name, const char *server_url) {
    char url_buffer[1024];
    char query_params[512] = "";
    int offset = 0;
    offset += snprintf(query_params + offset, sizeof(query_params) - offset, "?");
    if (start_time > 0) offset += snprintf(query_params + offset, sizeof(query_params) - offset, "start_time=%lld&", start_time);
    if (end_time > 0) offset += snprintf(query_params + offset, sizeof(query_params) - offset, "end_time=%lld&", end_time);
    if (pid > 0) offset += snprintf(query_params + offset, sizeof(query_params) - offset, "pid=%d&", pid);
    if (name) offset += snprintf(query_params + offset, sizeof(query_params) - offset, "name=%s&", name);
    if (offset > 1) query_params[offset - 1] = '\0'; // 移除最后一个 '&' 或 '?'

    snprintf(url_buffer, sizeof(url_buffer), "%s/query%s", server_url, query_params);
    fprintf(stderr, "DEBUG: 实时查询 URL: %s\n", url_buffer);

    CURL *curl;
    CURLcode res;
    MemoryStruct chunk = { .memory = malloc(1), .size = 0 }; // 初始化内存缓冲区

    curl_global_init(CURL_GLOBAL_ALL); // 初始化 cURL 全局环境
    curl = curl_easy_init();           // 获取 cURL 句柄
    if (!curl) {
        free(chunk.memory);
        curl_global_cleanup();
        return 1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url_buffer);             // 设置请求 URL
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback); // 设置写入回调函数
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);   // 设置回调函数的用户数据
    // 强制禁用代理
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    res = curl_easy_perform(curl);                               // 执行 HTTP 请求

    if (res != CURLE_OK) {
        fprintf(stderr, "HTTP请求失败: %s\n", curl_easy_strerror(res));
    } else {
        cJSON *json = cJSON_Parse(chunk.memory); // 解析 JSON 响应
        if (json == NULL) {
            fprintf(stderr, "JSON解析失败: %s\n", cJSON_GetErrorPtr());
        } else {
            ht_t* results = ht_create(); // 创建哈希表以聚合结果
            cJSON *item;
            // 遍历 JSON 数组中的每个项目
            cJSON_ArrayForEach(item, json) {
                cJSON *stack = cJSON_GetObjectItemCaseSensitive(item, "stack");
                cJSON *count = cJSON_GetObjectItemCaseSensitive(item, "count");
                if (cJSON_IsString(stack) && cJSON_IsNumber(count)) {
                    ht_insert(results, stack->valuestring, count->valueint); // 插入或累加到哈希表
                }
            }
            ht_foreach(results); // 打印聚合结果
            ht_destroy(results); // 销毁哈希表
            cJSON_Delete(json);  // 释放 cJSON 对象
        }
    }
    curl_easy_cleanup(curl);     // 清理 cURL 句柄
    free(chunk.memory);          // 释放内存缓冲区
    curl_global_cleanup();       // 清理 cURL 全局环境
    return res == CURLE_OK ? 0 : 1;
}

// --- 历史查询 (SQLite模式) ---
/**
 * @brief 执行历史性能数据查询。
 *
 * 遍历指定目录下的 SQLite 数据库文件，根据时间范围和过滤条件查询数据，
 * 并聚合结果。
 *
 * @param db_dir 存储 SQLite 数据库文件的目录。
 * @param start_time 查询的开始时间戳 (Unix秒)。
 * @param end_time 查询的结束时间戳 (Unix秒)。
 * @param pid 要过滤的进程ID (0 表示不过滤)。
 * @param name 要过滤的进程名称 (NULL 表示不过滤)。
 * @return 0 表示成功，非 0 表示失败。
 */
static int perform_historical_query(const char *db_dir, long long start_time, long long end_time, int pid, const char *name) {
    ht_t* aggregated_results = ht_create(); // 创建哈希表以聚合所有数据库的查询结果
    // 计算开始时间所在的小时的时间戳
    time_t current_hour_ts = start_time - (start_time % 3600);

    // 遍历每个小时的数据库文件
    while (current_hour_ts <= end_time) {
        struct tm tm;
        localtime_r(&current_hour_ts, &tm); // 将时间戳转换为本地时间结构
        char db_path[256];
        // 构建数据库文件路径，格式为 profiling_YYYY-MM-DD_HH.db
        snprintf(db_path, sizeof(db_path), "%s/profiling_%04d-%02d-%02d_%02d.db",
                 db_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour);
        fprintf(stderr, "DEBUG: Checking DB path: %s\n", db_path);

        // 检查数据库文件是否存在
        if (access(db_path, F_OK) != 0) {
            fprintf(stderr, "DEBUG: DB file not found: %s\n", db_path);
            current_hour_ts += 3600; // 移动到下一个小时
            continue;
        }

        sqlite3 *db;
        // 以只读模式打开 SQLite 数据库
        if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL)) {
            fprintf(stderr, "DEBUG: 无法打开数据库: %s (%s)\n", db_path, sqlite3_errmsg(db));
            current_hour_ts += 3600; // 移动到下一个小时
            continue;
        }
        fprintf(stderr, "DEBUG: Opened DB: %s\n", db_path);

        char sql[1024];
        char where_clauses[512] = " WHERE 1=1 "; // 构建 WHERE 子句
        int offset = strlen(where_clauses);
        // 添加时间戳过滤条件 (纳秒级)
        if (start_time > 0) offset += snprintf(where_clauses + offset, sizeof(where_clauses) - offset, " AND timestamp >= %lld", start_time * 1000000000LL);
        if (end_time > 0) offset += snprintf(where_clauses + offset, sizeof(where_clauses) - offset, " AND timestamp <= %lld", end_time * 1000000000LL);
        // 添加 PID 过滤条件
        if (pid > 0) offset += snprintf(where_clauses + offset, sizeof(where_clauses) - offset, " AND pid = %d", pid);
        // 添加进程名称过滤条件
        if (name) offset += snprintf(where_clauses + offset, sizeof(where_clauses) - offset, " AND process_name = '%s'", name);
        // 构建最终的 SQL 查询语句
        snprintf(sql, sizeof(sql), "SELECT pid, process_name, full_stack, COUNT(*) FROM call_stacks %s GROUP BY pid, process_name, full_stack;", where_clauses);
        fprintf(stderr, "DEBUG: SQL Query: %s\n", sql);

        sqlite3_stmt *stmt;
        // 准备 SQL 语句
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
            fprintf(stderr, "DEBUG: SQL准备失败: %s\n", sqlite3_errmsg(db));
        } else {
            // 遍历查询结果
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                fprintf(stderr, "DEBUG: Found a row.\n");
                const unsigned char *full_stack = sqlite3_column_text(stmt, 2); // 获取完整调用栈
                int count = sqlite3_column_int(stmt, 3);                 // 获取计数
                if (full_stack) ht_insert(aggregated_results, (const char*)full_stack, count); // 插入或累加到哈希表
            }
            sqlite3_finalize(stmt); // 释放 SQL 语句句柄
        }
        sqlite3_close(db); // 关闭数据库
        current_hour_ts += 3600; // 移动到下一个小时
    }

    ht_foreach(aggregated_results); // 打印聚合结果
    ht_destroy(aggregated_results); // 销毁哈希表
    return 0;
}

/**
 * @brief 打印程序的用法信息。
 *
 * @param prog_name 程序的名称。
 */
void print_usage(const char *prog_name) {
    fprintf(stderr, "用法: %s [选项]\n", prog_name);
    fprintf(stderr, "模式:\n");
    fprintf(stderr, "  历史查询: --dir <数据库目录> [时间/过滤选项]\n");
    fprintf(stderr, "  实时查询: [时间/过滤选项] [--server <url>]\n\n");
    fprintf(stderr, "选项:\n");
    fprintf(stderr, "  --start-time <时间戳>  开始时间 (Unix秒)\n");
    fprintf(stderr, "  --end-time <时间戳>    结束时间 (Unix秒)\n");
    fprintf(stderr, "  --pid <pid>            按进程ID过滤\n");
    fprintf(stderr, "  --name <名称>          按进程名过滤\n");
    fprintf(stderr, "  --server <url>         (实时模式)服务器地址 (默认: http://localhost:8081)\n");
}

/**
 * @brief 程序主函数。
 *
 * 解析命令行参数，并根据参数选择执行历史查询或实时查询。
 *
 * @param argc 命令行参数的数量。
 * @param argv 命令行参数数组。
 * @return 程序的退出码。
 */
int main(int argc, char *argv[]) {
    const char *db_dir = NULL;
    long long start_time = 0;
    long long end_time = 0;
    int pid = 0;
    const char *name = NULL;
    const char *server_url = "http://localhost:8081"; // 默认实时查询服务器地址

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) db_dir = argv[++i];
        else if (strcmp(argv[i], "--start-time") == 0 && i + 1 < argc) start_time = atoll(argv[++i]);
        else if (strcmp(argv[i], "--end-time") == 0 && i + 1 < argc) end_time = atoll(argv[++i]);
        else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) pid = atoi(argv[++i]);
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
        else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) server_url = argv[++i];
        else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    // 如果未指定结束时间，则默认为当前时间
    if (end_time == 0) end_time = time(NULL);

    if (db_dir) {
        // 历史模式：从数据库目录查询
        return perform_historical_query(db_dir, start_time, end_time, pid, name);
    } else {
        // 实时模式：通过 HTTP 请求查询
        return perform_live_query(start_time, end_time, pid, name, server_url);
    }
}