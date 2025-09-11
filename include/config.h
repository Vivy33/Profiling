#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <sys/types.h>

// 性能分析模式
typedef enum {
    MODE_SYSTEM,    // 系统级监控 - 监控所有进程
    MODE_TARGET,    // 目标进程模式 - 监控单个指定进程
    MODE_MULTI      // 多目标模式 - 监控多个指定进程
} profiling_mode_t;

// 目标进程信息
struct target_process {
    pid_t pid;              // 进程ID (-1表示需要启动)
    char* exec_path;        // 可执行文件路径
    char* args;             // 启动参数
    bool auto_restart;      // 进程退出后自动重启
};

// 性能分析配置
struct profiling_config {
    profiling_mode_t mode;              // 分析模式
    struct target_process* targets;     // 目标进程数组
    int target_count;                   // 目标进程数量
    int sampling_frequency;             // 采样频率 (Hz)
    int sample_period_ns;               // 采样周期 (纳秒)
    bool exclude_kernel;                // 排除内核空间
    bool exclude_hypervisor;            // 排除虚拟机监控器
    int cleanup_interval;               // 死进程清理间隔 (秒)
    bool verbose;                       // 详细输出
};

// 函数声明
void print_usage(const char* program_name);
int parse_command_line(int argc, char* argv[], struct profiling_config* config);
void free_config(struct profiling_config* config);
int validate_config(const struct profiling_config* config);
const char* get_mode_name(profiling_mode_t mode);
void print_config_summary(const struct profiling_config* config);

// 默认配置
#define DEFAULT_SAMPLING_FREQUENCY 30    // 30Hz
#define DEFAULT_CLEANUP_INTERVAL 5       // 5秒
#define MAX_TARGETS 32                   // 最大目标进程数

#endif // CONFIG_H