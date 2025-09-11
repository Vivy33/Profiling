#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>

#include "../include/perf.h"
#include "../include/config.h"

// Wrapper for the perf_event_open system call
static long perf_event_open_syscall(struct perf_event_attr *hw_event, pid_t pid,
                                  int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

// 初始化性能事件（支持多种模式）
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
    
    // 计算需要的事件数量
    int num_events = 0;
    switch (config->mode) {
        case MODE_SYSTEM:
            // 每个CPU一个事件，监控所有进程
            num_events = num_cpus;
            break;
            
        case MODE_TARGET:
        case MODE_MULTI:
            // 每个CPU每个目标进程一个事件
            num_events = num_cpus * config->target_count;
            break;
    }

    struct perf_event_manager* manager = calloc(1, sizeof(struct perf_event_manager));
    if (!manager) {
        perror("calloc manager");
        return NULL;
    }

    manager->events = calloc(num_events, sizeof(struct perf_event_fd));
    if (!manager->events) {
        perror("calloc events");
        free(manager);
        return NULL;
    }

    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.type = PERF_TYPE_SOFTWARE;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = PERF_COUNT_SW_CPU_CLOCK;
    pe.sample_period = config->sample_period_ns;
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
    pe.disabled = 1;
    pe.exclude_kernel = config->exclude_kernel ? 1 : 0;
    pe.exclude_hv = config->exclude_hypervisor ? 1 : 0;

    int event_index = 0;
    
    // 根据模式创建事件
    switch (config->mode) {
        case MODE_SYSTEM:
            // 系统模式：每个CPU一个事件，监控所有进程
            for (int cpu = 0; cpu < num_cpus; cpu++) {
                manager->events[event_index].fd = perf_event_open_syscall(&pe, -1, cpu, -1, 0);
                if (manager->events[event_index].fd == -1) {
                    fprintf(stderr, "Error opening perf event for CPU %d: %s\n", 
                            cpu, strerror(errno));
                    goto error_cleanup;
                }
                manager->events[event_index].cpu = cpu;
                manager->events[event_index].target_pid = -1;
                
                // 设置非阻塞模式，用于边缘触发epoll
                fcntl(manager->events[event_index].fd, F_SETFL, O_NONBLOCK);
                
                ioctl(manager->events[event_index].fd, PERF_EVENT_IOC_RESET, 0);
                ioctl(manager->events[event_index].fd, PERF_EVENT_IOC_ENABLE, 0);
                event_index++;
            }
            break;

        case MODE_TARGET:
        case MODE_MULTI:
            // 目标模式：每个CPU每个目标进程一个事件
            for (int target_idx = 0; target_idx < config->target_count; target_idx++) {
                pid_t target_pid = config->targets[target_idx].pid;
                
                for (int cpu = 0; cpu < num_cpus; cpu++) {
                    manager->events[event_index].fd = perf_event_open_syscall(&pe, target_pid, cpu, -1, 0);
                    if (manager->events[event_index].fd == -1) {
                        fprintf(stderr, "Error opening perf event for PID %d CPU %d: %s\n", 
                                target_pid, cpu, strerror(errno));
                        goto error_cleanup;
                    }
                    manager->events[event_index].cpu = cpu;
                    manager->events[event_index].target_pid = target_pid;
                    
                    // 设置非阻塞模式，用于边缘触发epoll
                    fcntl(manager->events[event_index].fd, F_SETFL, O_NONBLOCK);
                    
                    ioctl(manager->events[event_index].fd, PERF_EVENT_IOC_RESET, 0);
                    ioctl(manager->events[event_index].fd, PERF_EVENT_IOC_ENABLE, 0);
                    event_index++;
                }
            }
            break;
    }

    manager->num_events = num_events;
    manager->num_cpus = num_cpus;

    if (config->verbose) {
        printf("Initialized %d perf events for %s mode\n", num_events, 
               config->mode == MODE_SYSTEM ? "system" : 
               (config->mode == MODE_TARGET ? "target" : "multi"));
        printf("Sampling frequency: %d Hz, period: %d ns\n", 
               config->sampling_frequency, config->sample_period_ns);
    }

    return manager;

error_cleanup:
    // 清理已创建的事件
    for (int i = 0; i < event_index; i++) {
        if (manager->events[i].fd >= 0) {
            ioctl(manager->events[i].fd, PERF_EVENT_IOC_DISABLE, 0);
            close(manager->events[i].fd);
        }
    }
    free(manager->events);
    free(manager);
    return NULL;
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

// 兼容旧接口
struct perf_event_fd* perf_event_init(int *num_cpus) {
    struct profiling_config config;
    struct profiling_config* cfg = &config;
    
    // 创建默认系统模式配置
    cfg->mode = MODE_SYSTEM;
    cfg->targets = NULL;
    cfg->target_count = 0;
    cfg->sampling_frequency = 30;
    cfg->sample_period_ns = 1000000000 / 30;
    cfg->exclude_kernel = true;
    cfg->exclude_hypervisor = true;
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
    }
    free(events);
}