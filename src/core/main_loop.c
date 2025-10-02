#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../include/header.h"
#include "../../include/config.h"
#include "../../include/perf.h"
#include "../../include/mempool.h"

struct mempool_s *sample_pool;

/**
 * @brief perf事件分发回调函数 (生产者)
 *
 * 该函数作为 `perf_event_consume_ring_buffer` 的回调，在每次从ring buffer中读取到一个
 * perf事件时被调用。它扮演了生产者角色，负责处理原始的 `PERF_RECORD_SAMPLE` 事件。
 *
 * 工作流程:
 * 1. 检查事件类型是否为 `PERF_RECORD_SAMPLE`。
 * 2. 调用 `parse_sample_data` 从事件的原始数据中解析出调用栈、PID、TID等信息。
 * 3. 从 `sample_pool` 内存池中申请一个 `raw_sample` 对象，以避免频繁 `malloc`。
 * 4. 如果内存池耗尽，则打印错误并丢弃该样本，以保证系统在高负载下仍能稳定运行。
 * 5. 将解析出的数据从临时结果 (`callchain_result`) 复制到 `raw_sample` 对象中。
 * 6. 将填充好的 `raw_sample` 推入 `symbolizer_queue` 并发队列，交由消费者线程处理。
 *
 * @param header 指向当前perf事件头的指针。
 * @param context 指向 `system_context` 的通用指针，包含了队列和内存池等共享资源。
 */
static void dispatch_sample_event(struct perf_event_header *header, void *context) {
    struct system_context *sys_info = (struct system_context *)context;
    
    // 我们只关心采样记录
    if (header->type == PERF_RECORD_SAMPLE) {
        struct callchain_result result;
        uint64_t ips_buffer[MAX_STACK_DEPTH_COPY];
        result.ips = ips_buffer;

        parse_sample_data(header, &result, MAX_STACK_DEPTH_COPY);

        struct raw_sample *sample = (struct raw_sample *)mempool_alloc(sys_info->sample_pool);
        if (sample == NULL) {
            // 当内存池耗尽时，打印错误并丢弃样本。在高负载下，这可以防止性能下降。
            fprintf(stderr, "Failed to allocate memory from pool for raw_sample, dropping sample.\n");
            return;
        }

        sample->timestamp_ns = result.timestamp_ns;
        sample->pid = result.pid;
        sample->tid = result.tid;
        sample->ip = result.ip;
        sample->nr = (result.nr < MAX_STACK_DEPTH_COPY) ? result.nr : MAX_STACK_DEPTH_COPY;

        // 使用 memcpy 进行调用栈的深拷贝。
        // 这里的源（result.ips）和目标（sample->ips）内存区域是完全独立的，
        // 不存在重叠的风险，因此使用 memcpy 是安全且高效的。
        // 相比于 for 循环，memcpy 通常能获得更好的性能，因为它能利用底层的硬件优化。
        // 复制的字节数由实际的调用栈深度（sample->nr）决定，确保了操作的安全性，不会导致缓冲区溢出。
        memcpy(sample->ips, result.ips, sample->nr * sizeof(uint64_t));

        // 将填充好数据的样本推入并发队列，供符号化线程消费。
        // 该队列的实现保证了线程安全。如果队列已满，它可能会阻塞或丢弃样本，
        // 具体行为取决于其内部实现，但此处我们假设它能正确处理。
        queue_push(sys_info->symbolizer_queue, sample);
    }
}

/**
 * @brief 主事件循环，负责从perf_event中读取和处理性能数据。
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
if (total_events_processed > 0) {
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