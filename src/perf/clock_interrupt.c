#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/stat.h>

static int perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

volatile int sample_count = 0;

void sample_handler(int signum) {
    (void)signum;  // 避免未使用变量的警告
    sample_count++;
    char path[256];
    char comm[256];
    int pid;
    
    // 获取当前进程ID和进程名
    snprintf(path, sizeof(path), "/proc/self/task/%d/comm", getpid());
    FILE *comm_file = fopen(path, "r");
    if (comm_file) {
        fscanf(comm_file, "%255s", comm); // 读取时限制长度避免溢出
        fclose(comm_file);
    } else {
        strcpy(comm, "unknown");
    }
    
    // 获取当前进程ID
    pid = getpid();

    // 打印进程信息
    printf("CPU Clock Interrupt: PID: %d, Process: %s, Sample Count: %d\n", pid, comm, sample_count);
}

int main() {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.type = PERF_TYPE_SOFTWARE;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = PERF_COUNT_SW_CPU_CLOCK; // 使用 CPU 时钟中断作为触发源
    pe.disabled = 1;
    pe.exclude_kernel = 0;
    pe.exclude_hv = 1;
    pe.sample_period = 10000000; // 每10^7个时钟周期采样一次，即每秒采样100次
    pe.sample_type = PERF_SAMPLE_TID;

    int fd = perf_event_open(&pe, -1, 0, -1, 0);
    if (fd == -1) {
        perror("perf_event_open");
        exit(EXIT_FAILURE);
    }

    // 使用 signal 函数设置简单的信号处理程序
    signal(SIGIO, sample_handler);

    fcntl(fd, F_SETFL, O_RDWR | O_NONBLOCK | O_ASYNC);
    fcntl(fd, F_SETOWN, getpid());

    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

    // 让程序运行一段时间以捕获数据
    sleep(10);

    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    close(fd);

    // 扫描和清理进程
    scan_processes();
    
    return 0;
}