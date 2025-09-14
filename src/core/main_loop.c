#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "../../include/header.h"
#include "../../include/config.h"
#include "../../include/perf.h"

// epoll 水平触发模式：只要fd上有未消费的数据，就会持续触发epoll_wait
// 这样可以确保所有数据都被正确处理，避免边缘触发可能导致的数据丢失
// 虽然性能略低于边缘触发，但保证了数据完整性和可靠性

// 配置epoll描述符
static int setup_epoll_fd(struct perf_event_manager* manager) {
    if (!manager) return -1;
    
    // 创建epoll实例，使用EPOLL_CLOEXEC避免fork后继承
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        return -1;
    }
    
    // 为每个perf事件文件描述符注册到epoll
    for (int i = 0; i < manager->num_events; i++) {
        struct epoll_event event;
        event.events = EPOLLIN;  // 水平触发模式，确保数据完整性
        event.data.fd = manager->events[i].fd;  // 存储文件描述符用于后续处理
        
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, manager->events[i].fd, &event) == -1) {
            perror("epoll_ctl failed");
            close(epoll_fd);
            return -1;
        }
    }
    
    return epoll_fd;
}

/**
 * @brief 主事件循环
 * @param system_info 指向system_context结构体的指针，包含进程哈希表和ELF文件缓存等系统全局信息。
 * @param manager 指向perf_event_manager结构体的指针，包含所有perf事件的文件描述符和元数据。
 */
void main_loop(struct system_context* system_info, struct perf_event_manager* manager) {
    extern struct profiling_config global_config;
    
    // 设置epoll文件描述符
    int epoll_fd = setup_epoll_fd(manager);
    if (epoll_fd == -1) {
        return;
    }

    // 为epoll_wait准备事件缓冲区，使用批量处理提高效率
    struct epoll_event events[64];
    char buf[4096];
    struct sample_data *data;
    time_t last_cleanup_time = time(NULL);

    printf("Starting profiling with epoll... Press Ctrl+C to stop\n\n");

    while (1) {
        // 使用epoll_wait等待事件，支持毫秒级超时
        int num_events = epoll_wait(epoll_fd, events, 
                                   sizeof(events)/sizeof(events[0]),
                                   global_config.cleanup_interval * 1000);
        
        if (num_events == -1) {
            if (errno == EINTR) {
                continue; // 被信号中断，继续循环
            }
            perror("epoll_wait failed");
            break;
        }
        // 使用水平触发epoll模式处理perf事件，确保数据完整性
        for (int i = 0; i < num_events; i++) {
            if (events[i].events & EPOLLIN) {
                // 对于每个有数据的CPU，处理其ring buffer
                // 这里我们仍然使用read方式，但已优化为水平触发模式
                int n = read(events[i].data.fd, buf, sizeof(buf));
                if (n > 0) {
                    for (int j = 0; j < n; ) {
                        struct perf_event_header *header = (struct perf_event_header *)(buf + j);
                        if (header->type == PERF_RECORD_SAMPLE) {
                            data = (struct sample_data *)header;
                            handle_sample(system_info, data);
                        }
                        j += header->size;
                    }
                }
            }
        }

        // 定期清理已退出的进程（即使没有事件发生也执行）
        time_t current_time = time(NULL);
        if (current_time - last_cleanup_time >= global_config.cleanup_interval) {
            cleanup_dead_processes(system_info);
            last_cleanup_time = current_time;
        }
    }

    close(epoll_fd);  // 清理epoll文件描述符
}
