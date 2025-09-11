#ifndef PERF_H
#define PERF_H

#include <stdint.h>
#include <sys/types.h>

// Forward declaration
struct profiling_config;

// Perf event file descriptor for a single CPU
struct perf_event_fd {
    int fd;
    int cpu;
    pid_t target_pid;  // 目标进程PID (-1表示所有进程)
    // Add other necessary fields, e.g., for mmap buffer
};

// Perf event manager for different profiling modes
struct perf_event_manager {
    struct perf_event_fd* events;
    int num_events;
    int num_cpus;
};

// 初始化性能事件（支持多种模式）
// config: 配置信息，包含目标进程等
// 返回: 性能事件管理器，NULL表示失败
struct perf_event_manager* perf_event_init_with_config(const struct profiling_config* config);

// 清理性能事件
void perf_event_cleanup_manager(struct perf_event_manager* manager);

// 兼容旧接口（系统模式）
struct perf_event_fd* perf_event_init(int *num_cpus);
void perf_event_cleanup(struct perf_event_fd *events, int num_cpus);

#endif // PERF_H
