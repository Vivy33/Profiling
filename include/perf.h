#ifndef PERF_H
#define PERF_H

#include <stdint.h>
#include <sys/types.h>
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <stdatomic.h>

// 前向声明
struct profiling_config;
struct sample_data;


/**
 * @brief 单个CPU的Perf事件文件描述符结构
 *
 * 封装了与单个CPU上的perf事件相关的所有信息，形成了从内核到用户空间的完整桥梁。
 * 每个CPU核心对应一个此结构体实例。
 *
 * 数据流：
 * 内核事件 → fd → mmap_page → mmap_buffer → 用户处理
 */
struct perf_event_fd {
    int fd;                     /** @brief perf事件文件描述符
                                 *   - 来源：perf_event_open系统调用返回
                                 *   - 用途：控制事件（启用/禁用/重置）
                                 *   - 生命周期：创建到perf_event_cleanup_manager */

    int cpu;                    /** @brief 绑定的CPU核心ID
                                 *   - 范围：0到num_cpus-1
                                 *   - 作用：指定在哪个CPU核心上采样
                                 *   - 系统级采样时：每个CPU一个事件 */

    pid_t target_pid;           /** @brief 目标进程PID
                                 *   - -1: 系统级采样（监控所有进程）
                                 *   - 0: 当前进程
                                 *   - >0: 特定进程PID（进程级采样） */

    struct perf_event_mmap_page *mmap_page; /** @brief 映射的perf事件元数据页
                                           *   - 结构：包含data_head/data_tail等同步字段
                                           *   - 作用：内核与用户空间同步的桥梁
                                           *   - 内存：mmap映射的第一个页 */

    char *mmap_buffer;          /** @brief 数据缓冲区起始地址
                                 *   - 计算：mmap_page + pagesize
                                 *   - 大小：1MB环形缓冲区
                                 *   - 用途：存储实际的perf事件数据 */

    size_t mmap_size;           /** @brief 映射区域总大小
                                 *   - 值：1MB + pagesize（1MB数据 + 1页元数据）
                                 *   - 用途：munmap清理时的参数 */
    size_t pagesize;            /** @brief 系统页大小
                                 *   - 来源：sysconf(_SC_PAGESIZE)
                                 *   - 作用：缓存页大小，避免重复系统调用 */
};

/**
 * @brief Perf事件管理器结构
 *
 * 统一管理所有CPU核心的perf事件，支持系统级和进程级性能分析。
 * 提供了完整的perf事件生命周期管理功能。
 *
 * 生命周期管理：
 * perf_event_init_with_config() → 创建 → 使用 → perf_event_cleanup_manager() → 销毁
 */
struct perf_event_manager {
    struct perf_event_fd* events; /** @brief perf事件数组
                                   *   - 数组大小：num_cpus
                                   *   - 索引：events[cpu_id] 对应CPU核心
                                   *   - 生命周期：由perf_event_init_with_config分配
                                   *   - 清理：由perf_event_cleanup_manager释放 */

    int num_events;               /** @brief 当前管理的事件数量
                                   *   - 值：等于num_cpus（每个CPU一个事件）
                                   *   - 用途：数组边界检查 */

    int num_cpus;                 /** @brief 系统中的CPU核心数量
                                   *   - 来源：sysconf(_SC_NPROCESSORS_ONLN)
                                   *   - 用途：决定events数组大小和循环边界 */
};

// 缓冲区管理函数
static inline char *perf_get_mmap_buf(struct perf_event_mmap_page *page, size_t pagesize) {
    return (char *)page + pagesize;
}


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
 * @return 返回处理的事件数量，0表示没有新数据
 */
int perf_event_consume_ring_buffer(struct perf_event_fd *event, 
                                     void (*handler)(struct perf_event_header *, void *), 
                                     void *context);



#endif // PERF_H
