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
#include <sys/mman.h>
#include <stdatomic.h>

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
    
    pe.type = PERF_TYPE_SOFTWARE;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = PERF_COUNT_SW_CPU_CLOCK;
    
    // 使用频率模式替代周期模式
    pe.freq = 1;
    pe.sample_freq = config->sampling_frequency;
    
    // 使用callchain + LBR + reg进行栈回溯，不依赖libunwind
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN | 
                     PERF_SAMPLE_BRANCH_STACK | PERF_SAMPLE_REGS_USER;
    
    pe.disabled = 1;
    pe.exclude_kernel = 0;  // 全量采集，包含用户态和内核态
    
    return pe;
}

// 创建单个perf事件
static int create_perf_event(struct perf_event_attr *pe, int cpu, pid_t target_pid) {
    return perf_event_open_syscall(pe, target_pid, cpu, -1, 0);
}

// 设置事件内存映射
static int setup_event_mmap(struct perf_event_fd *event) {
    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t mmap_size = (1 + 16) * page_size; // 1页头 + 16页数据 (64KB)

    // 使用MAP_SHARED按需映射，替代MAP_POPULATE预加载
    void *mmap_base = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED,
                          event->fd, 0);
    if (mmap_base == MAP_FAILED) {
        fprintf(stderr, "Error mmap'ing perf event for CPU %d: %s\n",
                event->cpu, strerror(errno));
        return -1;
    }

    event->mmap_base = mmap_base;
    event->mmap_size = mmap_size;
    event->header = (struct perf_event_mmap_page *)mmap_base;

    // 设置环形缓冲区水印，当数据达到75%时唤醒
    uint64_t wakeup_watermark = (mmap_size - page_size) * 3 / 4;
    (void)wakeup_watermark; // 避免未使用警告

    // 这里可以设置实际的水印值，但暂时保持简单实现

    return 0;
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
        if (manager->events[i].mmap_base) {
            munmap(manager->events[i].mmap_base, manager->events[i].mmap_size);
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
        
        if (setup_event_mmap(&manager->events[cpu]) != 0) {
            cleanup_events(manager, cpu + 1);
            return NULL;
        }
        
        // 设置CPU亲和性
        if (perf_event_bind_to_cpu(&manager->events[cpu], cpu) != 0) {
            fprintf(stderr, "Warning: Failed to bind CPU %d affinity\n", cpu);
        }
        
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
            if (manager->events[i].mmap_base) {
                munmap(manager->events[i].mmap_base, manager->events[i].mmap_size);
            }
        }
        free(manager->events);
    }
    
    free(manager);
}

// 兼容旧接口
struct perf_event_fd* perf_event_init(int *num_cpus) {
    struct profiling_config config;
    struct profiling_config* cfg = &config;
    
    // 简化配置，只保留必要的采样频率
    cfg->sampling_frequency = 30;
    cfg->verbose = false;
    
    struct perf_event_manager* manager = perf_event_init_with_config(cfg);
    if (!manager) return NULL;
    
    *num_cpus = manager->num_cpus;
    
    // 返回兼容的事件数组
    struct perf_event_fd* events = manager->events;
    free(manager); // 只释放管理器，保留事件数组
    
    return events;
}

void perf_event_cleanup(struct perf_event_fd *events, int num_cpus) {
    if (!events) return;
    
    for (int i = 0; i < num_cpus; i++) {
        if (events[i].fd >= 0) {
            ioctl(events[i].fd, PERF_EVENT_IOC_DISABLE, 0);
            close(events[i].fd);
        }
        if (events[i].mmap_base) {
            munmap(events[i].mmap_base, events[i].mmap_size);
        }
    }
    free(events);
}

// CPU亲和性绑定 - 确保CPU本地处理
int perf_event_bind_to_cpu(struct perf_event_fd *event, int cpu) {
    if (!event) return -1;
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    
    // 设置线程CPU亲和性
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        perror("pthread_setaffinity_np");
        return -1;
    }
    
    return 0;
}

// 设置CPU亲和性
int perf_event_set_cpu_affinity(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    
    return sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

// 基于参考实现consume_all_perf_event的lock-free ring buffer处理
int perf_event_process_ring_buffer(struct perf_event_fd *event, void (*handler)(struct perf_event_header *)) {
    if (!event || !handler || !event->header) {
        return 0;
    }

    struct perf_event_mmap_page *header = event->header;
    char *base = (char *)event->mmap_base + sysconf(_SC_PAGESIZE);
    
    // 使用atomic_load确保原子读取
    uint64_t head = atomic_load(&header->data_head);
    uint64_t tail = atomic_load(&header->data_tail);
    
    if (head == tail) {
        return 0; // 没有新数据
    }
    
    uint64_t buffer_size = event->mmap_size - sysconf(_SC_PAGESIZE);
    char *data_start = base;
    
    while (tail != head) {
        struct perf_event_header *event_header = (struct perf_event_header *)(data_start + (tail % buffer_size));
        
        // 检查事件是否完整
        if (event_header->size == 0) {
            break;
        }
        
        handler(event_header);
        tail += event_header->size;
    }
    
    atomic_store(&header->data_tail, head);
    return 1;
}

// 处理所有采样事件
int perf_event_consume_samples(struct perf_event_manager *manager, void (*handler)(struct sample_data *)) {
    if (!manager || !handler || !manager->events) {
        return -1;
    }

    int processed = 0;
    for (int i = 0; i < manager->num_events; i++) {
        processed += perf_event_process_ring_buffer(&manager->events[i], 
                                                   (void (*)(struct perf_event_header *))handler);
    }
    return processed;
}