#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
// #include <sys/time.h>

#include "../../include/header.h"
#include "../../include/config.h"
#include "../../include/perf.h"

// dispatch_sample_event 是一个回调函数，用于根据事件类型分发来自ring buffer的perf事件
static void dispatch_sample_event(struct perf_event_header *header, void *context) {
    struct system_context *sys_info = (struct system_context *)context;
    
    // 我们只关心采样记录
    if (header->type == PERF_RECORD_SAMPLE) {
        struct callchain_result result;
        // 创建一个安全的栈上缓冲区来存储调用栈IP。
        // 内核默认栈深度通常不超过127，我们这里设置一个安全的上限。
        uint64_t ips_buffer[128];
        result.ips = ips_buffer;

        // 将缓冲区和其大小传递给解析函数，以安全地复制数据
        parse_sample_data(header, &result, 128);
        symbolize_sample(sys_info, &result);
    }
}

/**
 * @brief 主事件循环 - 采用自适应休眠策略
 * @param system_info 指向system_context结构体的指针，包含进程哈希表和ELF文件缓存等系统全局信息。
 * @param manager 指向perf_event_manager结构体的指针，包含所有perf事件的文件描述符和元数据。
 */
void main_loop(struct system_context* system_info, struct perf_event_manager* manager) {
    extern struct profiling_config global_config;
    
    time_t last_cleanup_time = time(NULL);
// gettimeofday 量化时间 方便日后debug    
// struct timeval start_time, end_time;

    printf("Starting profiling with adaptive sleep loop... Press Ctrl+C to stop\n\n");

    while (1) {
// gettimeofday(&start_time, NULL);

        int total_events_processed = 0;
        // 遍历所有CPU核心的perf event fd，消费所有可用数据
        for (int i = 0; i < manager->num_events; i++) {
            total_events_processed += perf_event_consume_ring_buffer(&manager->events[i], dispatch_sample_event, system_info);
        }

// gettimeofday(&end_time, NULL);
/*
观察cpu利用率，核心多可能出现永远空转
if (global_config.verbose && total_events_processed > 0) {
    long seconds = end_time.tv_sec - start_time.tv_sec;
    long micros = ((seconds * 1000000) + end_time.tv_usec) - (start_time.tv_usec);
    printf("Processed %d events in %ld microseconds\n", total_events_processed, micros);
}
*/

        // 只有当一轮完整的检查没有发现任何新事件时，才进行休眠
        if (total_events_processed == 0) {
            // 定期清理已退出的进程，仅在系统空闲时执行
            time_t current_time = time(NULL);
            if (current_time - last_cleanup_time >= global_config.cleanup_interval) {
                cleanup_dead_processes(system_info);
                last_cleanup_time = current_time;
            }
            
            // 暂停100ms，避免CPU空转
            usleep(100000);
        }
        // 如果处理了事件，则立即再次循环，以尽快处理下一批数据
    }
}