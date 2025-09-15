#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <asm/perf_regs.h>
#include <sched.h>
#include <pthread.h>

#include "../include/perf.h"
#include "../include/config.h"

// perf_event_open系统调用的封装
static long perf_event_open_syscall(struct perf_event_attr *hw_event, pid_t pid,
                                  int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

// 构建perf事件属性配置
static struct perf_event_attr build_perf_attr(const struct profiling_config* config) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.size = sizeof(struct perf_event_attr);

    if (config->use_lbr) {
        // LBR模式: 使用硬件事件，采集分支记录
        pe.type = PERF_TYPE_HARDWARE;
        pe.config = PERF_COUNT_HW_INSTRUCTIONS;
        // 使用callchain + LBR + reg进行栈回溯，不依赖libunwind
        pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN | 
                         PERF_SAMPLE_BRANCH_STACK | PERF_SAMPLE_REGS_USER;
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

// 统一清理函数
static void cleanup_events(struct perf_event_manager* manager, int last_event) {
    if (!manager || !manager->events) return;
    
    for (int i = 0; i < last_event; i++) {
        if (manager->events[i].fd >= 0) {
            ioctl(manager->events[i].fd, PERF_EVENT_IOC_DISABLE, 0);
            close(manager->events[i].fd);
        }
    }
    free(manager->events);
    free(manager);
}

/**
 * @brief 初始化性能事件（支持多种模式）
 * @param config 指向profiling_config结构体的指针，包含采样频率、过滤模式等配置信息。
 * @return 成功时返回指向perf_event_manager结构体的指针，失败时返回NULL。
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

// 清理性能事件管理器
void perf_event_cleanup_manager(struct perf_event_manager* manager) {
    if (!manager) return; 
    
    if (manager->events) {
        for (int i = 0; i < manager->num_events; i++) {
            if (manager->events[i].fd >= 0) {
                ioctl(manager->events[i].fd, PERF_EVENT_IOC_DISABLE, 0);
                close(manager->events[i].fd);
            }
        }
        free(manager->events);
    }
    
    free(manager);
}

// 基于read的ring buffer消费
int perf_event_consume_ring_buffer(struct perf_event_fd *event, 
                                     void (*handler)(struct perf_event_header *, void *), 
                                     void *context) {
    if (!event || !handler) {
        return 0;
    }

    char buf[4096];
    ssize_t bytes_read = read(event->fd, buf, sizeof(buf));

    if (bytes_read <= 0) {
        return 0; // 没有数据或发生错误
    }

    char *ptr = buf;
    char *end = buf + bytes_read;
    int processed_count = 0;

    while (ptr < end) {
        struct perf_event_header *header = (struct perf_event_header *)ptr;
        if (header->size == 0 || (ptr + header->size > end)) {
            // 事件不完整，停止处理
            break;
        }
        
        handler(header, context);
        ptr += header->size;
        processed_count++;
    }

    return processed_count > 0 ? 1 : 0;
}
