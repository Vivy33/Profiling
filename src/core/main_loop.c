#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "../../include/header.h"
#include "../../include/config.h"
#include "../../include/perf.h"

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
        event.events = EPOLLIN | EPOLLET;  // 边缘触发模式，提高性能
        event.data.fd = manager->events[i].fd;  // 存储文件描述符用于后续处理
        
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, manager->events[i].fd, &event) == -1) {
            perror("epoll_ctl failed");
            close(epoll_fd);
            return -1;
        }
    }
    
    return epoll_fd;
}

// 主循环
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

        // 处理所有就绪的事件
        for (int i = 0; i < num_events; i++) {
            // 边缘触发模式：需要循环读取直到EAGAIN
            while (1) {
                int n = read(events[i].data.fd, buf, sizeof(buf));
                if (n == -1) {
                    if (errno == EAGAIN) {
                        // 边缘触发：数据已读完，退出循环
                        break;
                    } else if (errno != EINTR) {
                        // 真正的错误，打印并继续
                        perror("read");
                        break;
                    }
                    continue; // 被信号中断，重试
                } else if (n == 0) {
                    // 文件描述符关闭，这种情况在perf事件中不应该发生
                    break;
                }

                // 处理perf事件数据包
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

        // 定期清理已退出的进程（即使没有事件发生也执行）
        time_t current_time = time(NULL);
        if (current_time - last_cleanup_time >= global_config.cleanup_interval) {
            cleanup_dead_processes(system_info);
            last_cleanup_time = current_time;
        }
    }

    close(epoll_fd);  // 清理epoll文件描述符
}
