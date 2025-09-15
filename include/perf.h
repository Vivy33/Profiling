#ifndef PERF_H
#define PERF_H

#include <stdint.h>
#include <sys/types.h>
#include <linux/perf_event.h>
#include <sys/mman.h>

// 前向声明
struct profiling_config;
struct sample_data;

/**
 * @brief 单个CPU的Perf事件文件描述符结构
 * 
 * 封装了与单个CPU上的perf事件相关的所有信息，包括文件描述符、
 * CPU ID、目标进程PID、内存映射信息等。
 */
struct perf_event_fd {
    int fd;                   // Perf事件的文件描述符
    int cpu;                  // 该事件绑定的CPU ID
    pid_t target_pid;         // 目标进程PID (-1表示监控所有进程)
};

/**
 * @brief Perf事件管理器结构
 * 
 * 管理多个perf_event_fd，用于支持不同的性能分析模式（如系统级、多进程）。
 */
struct perf_event_manager {
    struct perf_event_fd* events; // 指向perf_event_fd数组的指针
    int num_events;               // 当前管理的事件数量
    int num_cpus;                 // 系统中的CPU数量
};

// 初始化性能事件（支持多种模式）
// config: 配置信息，包含目标进程等
// 返回: 性能事件管理器，NULL表示失败
struct perf_event_manager* perf_event_init_with_config(const struct profiling_config* config);

// 清理性能事件
void perf_event_cleanup_manager(struct perf_event_manager* manager);


/**
 * @brief 消费perf事件环形缓冲区中的数据 - lockfree
 * @param event 指向perf_event_fd结构体的指针
 * @param handler 处理perf_event_header的回调函数
 * @return 1表示处理了数据，0表示没有新数据
 */
int perf_event_consume_ring_buffer(struct perf_event_fd *event, 
                                     void (*handler)(struct perf_event_header *, void *), 
                                     void *context);



#endif // PERF_H
