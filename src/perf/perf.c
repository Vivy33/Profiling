/**
 * @file perf.c
 * @brief 基于mmap的高性能perf事件采集实现
 *
 * 本文件实现了基于Linux perf事件的性能分析系统，使用mmap映射内核环形缓冲区
 * 实现零拷贝数据采集。
 *
 * 数据流路径：
 * 内核perf事件 → mmap映射的环形缓冲区 → 用户空间处理 → 回调函数
 *
 * 关键概念：
 * - perf_event_mmap_page: 内核环形缓冲区元数据页
 * - data_head/data_tail: 生产者/消费者指针（内核/用户空间同步）
 * - 1MB+1页映射：1MB数据缓冲区 + 1页元数据
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdatomic.h>

#include "../include/perf.h"
#include "../include/config.h"

/**
 * @brief perf_event_open系统调用的封装函数
 * @param hw_event 指向perf事件配置结构体的指针
 *        - 包含事件类型、采样频率、采样类型等配置
 *        - 来源：用户配置或build_perf_attr函数生成
 * @param pid 目标进程PID
 *        - -1: 监控所有进程（系统级采样）
 *        - 0: 监控当前进程
 *        - >0: 监控指定PID的进程
 * @param cpu 目标CPU核心
 *        - -1: 监控所有CPU
 *        - ≥0: 监控指定CPU核心
 * @param group_fd 事件组文件描述符（-1表示不使用事件组）
 * @param flags 额外标志位（如PERF_FLAG_FD_CLOEXEC）
 * @return 成功返回文件描述符，失败返回-1
 */
static long perf_event_open_syscall(struct perf_event_attr *hw_event, pid_t pid,
                                    int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

/**
 * @brief 根据配置构建perf事件属性
 * @param config 指向profiling_config的指针
 *        - config->sampling_frequency: 采样频率（Hz）
 *        - config->use_lbr: 是否使用LBR模式
 *        - config->...: 其他配置参数
 * @return 返回配置好的perf_event_attr结构体
 *
 * 配置参数流转：
 * config参数 → perf_event_attr结构体 → perf_event_open_syscall → 内核事件
 */
static struct perf_event_attr build_perf_attr(const struct profiling_config* config) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.size = sizeof(struct perf_event_attr);

    if (config->use_lbr) {
        // LBR模式: 使用硬件事件，采集分支记录
        pe.type = PERF_TYPE_HARDWARE;
        pe.config = PERF_COUNT_HW_INSTRUCTIONS;
        // 使用LBR + reg进行栈回溯，不依赖libunwind，且只采集函数调用分支
        pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_BRANCH_STACK | PERF_SAMPLE_REGS_USER;
        pe.branch_sample_type = PERF_SAMPLE_BRANCH_ANY_CALL;
    } else {
        // 默认模式: 基于软件时钟
        pe.type = PERF_TYPE_SOFTWARE;
        pe.config = PERF_COUNT_SW_CPU_CLOCK;
        pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_REGS_USER;
    }
    
    // 使用频率模式
    pe.freq = 1;
    pe.sample_freq = config->sampling_frequency;
    
    pe.disabled = 1;
    pe.exclude_kernel = 0;  // 全量采集，包含用户态和内核态
    pe.exclude_idle = 1;    // 不采集IDLE的CPU
    
    return pe;
}

// 创建单个perf事件
static int create_perf_event(struct perf_event_attr *pe, int cpu, pid_t target_pid) {
    return perf_event_open_syscall(pe, target_pid, cpu, -1, 0);
}

// 初始化管理器结构
static struct perf_event_manager* initialize_manager(int num_cpus) {
    struct perf_event_manager* manager = calloc(1, sizeof(struct perf_event_manager));
    if (!manager) {
        perror("calloc manager");
        return NULL;
    }

    manager->events = calloc(num_cpus, sizeof(struct perf_event_fd));
    if (!manager->events) {
        perror("calloc events");
        free(manager);
        return NULL;
    }

    manager->num_events = num_cpus;
    manager->num_cpus = num_cpus;
    return manager;
}

/**
 * @brief 将perf事件fd映射到用户空间内存
 * @param fd perf事件的文件描述符
 *        - 来源：perf_event_open_syscall返回
 *        - 用途：用于mmap系统调用
 * @param mmap_size_out 用于返回实际映射大小的指针
 * @return 成功返回映射的perf_event_mmap_page指针，失败返回NULL
 *
 * 内存映射流程：
 * fd → mmap(1MB+1页) → perf_event_mmap_page* → perf_event_fd.mmap_page
 */
static struct perf_event_mmap_page* perf_add_sample_event_mmap(int fd, size_t *mmap_size_out) {
    long pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize < 0) {
        perror("sysconf pagesize");
        return NULL;
    }

    // 映射1MB + 1页（元数据页）
    *mmap_size_out = (1 * 1024 * 1024) + pagesize;
    void *buf = mmap(NULL, *mmap_size_out, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    struct perf_event_mmap_page *page = (struct perf_event_mmap_page *)buf;

    // 清理内核还未处理的事件
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

    return page;
}

// 统一清理函数
static void cleanup_events(struct perf_event_manager* manager, int last_event) {
    if (!manager || !manager->events) return;

    for (int i = 0; i < last_event; i++) {
        if (manager->events[i].fd >= 0) {
            ioctl(manager->events[i].fd, PERF_EVENT_IOC_DISABLE, 0);
            close(manager->events[i].fd);
        }
        if (manager->events[i].mmap_page) {
            munmap(manager->events[i].mmap_page, manager->events[i].mmap_size);
        }
    }
    free(manager->events);
    free(manager);
}

/**
 * @brief 初始化性能事件管理器（系统入口函数）
 * @param config 指向profiling_config的指针
 *        - config->sampling_frequency: 采样频率（Hz，如99, 1000）
 *        - config->use_lbr: 是否使用LBR模式（分支记录）
 *        - config->target_pid: 目标进程PID（-1为系统级，>0为特定进程）
 *        - 其他配置项：详见profiling_config结构体
 * @return 成功返回perf_event_manager*，失败返回NULL
 *
 * 初始化流程：
 * 1. 读取CPU数量 → 创建事件管理器
 * 2. 构建perf事件配置 → 创建事件
 * 3. 为每个CPU创建perf事件 → mmap映射缓冲区
 * 4. 返回管理器给用户
 */
struct perf_event_manager* perf_event_init_with_config(const struct profiling_config* config) {
    if (!config) {
        fprintf(stderr, "Error: NULL config provided\n");
        return NULL;
    }

    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs < 0) {
        perror("sysconf failed");
        return NULL;
    }

    int num_cpus = (int)nprocs;
    struct perf_event_manager* manager = initialize_manager(num_cpus);
    if (!manager) {
        return NULL;
    }

    struct perf_event_attr pe = build_perf_attr(config);
    
    // 系统模式：每个CPU一个事件，监控所有进程
    for (int cpu = 0; cpu < num_cpus; cpu++) {
        int fd = create_perf_event(&pe, cpu, -1);
        if (fd == -1) {
            fprintf(stderr, "Error opening perf event for CPU %d: %s\n",
                    cpu, strerror(errno));
            cleanup_events(manager, cpu);
            return NULL;
        }

        manager->events[cpu].fd = fd;
        manager->events[cpu].cpu = cpu;
        manager->events[cpu].target_pid = -1;

        // 使用mmap映射perf事件缓冲区
        manager->events[cpu].mmap_page = perf_add_sample_event_mmap(fd, &manager->events[cpu].mmap_size);
        if (!manager->events[cpu].mmap_page) {
            fprintf(stderr, "Error mapping perf buffer for CPU %d\n", cpu);
            cleanup_events(manager, cpu + 1);
            return NULL;
        }

        long pagesize = sysconf(_SC_PAGESIZE);
        if (pagesize < 0) {
            cleanup_events(manager, cpu + 1);
            return NULL;
        }

        manager->events[cpu].mmap_buffer = perf_get_mmap_buf(manager->events[cpu].mmap_page, pagesize);

        // 设置非阻塞模式
        fcntl(fd, F_SETFL, O_NONBLOCK);

        // 启用事件
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    }

    if (config->verbose) {
        printf("Initialized %d perf events\n", num_cpus);
        printf("Sampling frequency: %d Hz\n", config->sampling_frequency);
    }

    return manager;
}

/**
 * @brief 清理性能事件管理器
 */
void perf_event_cleanup_manager(struct perf_event_manager* manager) {
    if (!manager) return;

    if (manager->events) {
        for (int i = 0; i < manager->num_events; i++) {
            if (manager->events[i].fd >= 0) {
                ioctl(manager->events[i].fd, PERF_EVENT_IOC_DISABLE, 0);
                close(manager->events[i].fd);
            }
            if (manager->events[i].mmap_page) {
                munmap(manager->events[i].mmap_page, manager->events[i].mmap_size);
            }
        }
        free(manager->events);
    }

    free(manager);
}

/**
 * @brief 消费perf事件环形缓冲区中的数据（核心消费函数）
 * @param event 指向perf_event_fd结构体的指针
 *        - event->fd: perf事件文件描述符（用于验证）
 *        - event->mmap_page: 映射的perf事件元数据页
 *        - event->mmap_buffer: 数据缓冲区起始地址
 *        - event->mmap_size: 映射区域总大小
 * @param handler 处理perf_event_header的回调函数
 *        - 参数1: struct perf_event_header* - 事件头部指针
 *        - 参数2: void* - 用户提供的上下文指针
 *        - 返回值: 无（void）
 * @param context 用户上下文指针
 *        - 来源：用户调用时传入
 *        - 用途：传递给handler回调函数
 *        - 示例：统计信息、用户数据结构等
 * @return 返回处理的事件数量
 *         - 0: 无新数据或发生错误
 *         - >0: 成功处理的事件数量
 */
int perf_event_consume_ring_buffer(struct perf_event_fd *event, 
                                     void (*handler)(struct perf_event_header *, void *), 
                                     void *context) {
    if (!event || !handler || !event->mmap_page) {
        return 0;
    }

    long pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize < 0) {
        return 0;
    }

    // 计算数据缓冲区起始地址和大小
    char *base = perf_get_mmap_buf(event->mmap_page, pagesize);
    size_t buf_size = event->mmap_size - pagesize;  // 实际数据缓冲区大小

    // 原子读取生产者/消费者指针
    uint64_t tail = atomic_load(&event->mmap_page->data_tail);
    uint64_t head = atomic_load(&event->mmap_page->data_head);

    if (tail == head) {
        return 0; // 无新数据，立即返回
    }

    // 计算当前读取位置
    char *begin = base + (tail % buf_size);
    char *end = base + (head % buf_size);
    char *cur = begin;
    int processed_count = 0;

    // 临时缓冲区用于处理跨边界数据
    char temp_buf[4096];

    // 主循环：处理所有可用事件
    while (cur != end) {
        struct perf_event_header *hdr;

        // 检查事件头部是否跨边界
        if (cur + sizeof(struct perf_event_header) > base + buf_size) {
            size_t first_part = base + buf_size - cur;
            memcpy(temp_buf, cur, first_part);
            memcpy(temp_buf + first_part, base, sizeof(struct perf_event_header) - first_part);
            hdr = (struct perf_event_header *)temp_buf;
        } else {
            hdr = (struct perf_event_header *)cur;
        }

        if (hdr->size == 0) {
            break; // 无效事件，终止处理
        }

        // 处理事件数据的三种情况
        if (cur + hdr->size > base + buf_size) {
            size_t first_part = base + buf_size - cur;
            if (hdr->size <= sizeof(temp_buf)) {
                memcpy(temp_buf, cur, first_part);
                memcpy(temp_buf + first_part, base, hdr->size - first_part);
                handler((struct perf_event_header *)temp_buf, context);
                processed_count++;
            }
            cur = base + (hdr->size - first_part);
        } else {
            handler(hdr, context);
            processed_count++;
            cur += hdr->size;
            if (cur == base + buf_size) {
                cur = base;
            }
        }
    }

    // 原子更新tail指针，通知内核已消费到head位置
    atomic_store(&event->mmap_page->data_tail, head);

    return processed_count;
}
