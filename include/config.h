#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <sys/types.h>

// 过滤模式
enum filter_mode {
    FILTER_ALL,     // 显示全部
    FILTER_USER,    // 仅用户态
    FILTER_KERNEL   // 仅内核态
};

// 性能分析配置
struct profiling_config {
    int sampling_frequency;             // 采样频率 (Hz)
    enum filter_mode filter_mode;       // 显示过滤模式
    int cleanup_interval;               // 死进程清理间隔 (秒)
    int max_stack_depth;                // 最大栈回溯深度
    bool use_lbr;                       // 是否启用LBR
    char* db_output_dir;                // SQLite数据库输出目录
    int http_port;                      // HTTP服务器监听端口
    char* log_output_dir;               // 日志输出目录
};

// 函数声明
void print_usage(const char* program_name);
int parse_command_line(int argc, char* argv[], struct profiling_config* config);
int validate_config(struct profiling_config* config);

// 默认配置
#define DEFAULT_SAMPLING_FREQUENCY 30    // 30Hz
#define DEFAULT_CLEANUP_INTERVAL 30      // 30秒
#define DEFAULT_MAX_STACK_DEPTH 48       // 默认栈深度
#define DEFAULT_HTTP_PORT 8081           // 默认HTTP服务器端口
#define DEFAULT_LOG_DIR "/log"           // 默认日志输出目录
#define DEFAULT_DB_DIR "/tmp"            // 默认数据库输出目录
#define MAX_TARGETS 32                   // 最大目标进程数

#endif // CONFIG_H
