#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "include/database.h" // 包含db_writer_context_t的定义

// 前向声明MHD_Daemon结构体指针，避免在头文件中引入整个microhttpd.h
struct MHD_Daemon;

typedef struct http_server_context {
    struct MHD_Daemon *daemon;
    db_writer_context_t *db_context; // 指向数据库上下文的指针
} http_server_context_t;

http_server_context_t* http_server_start(int port, db_writer_context_t *db_context);
void http_server_stop(http_server_context_t *context);

#endif // HTTP_SERVER_H
