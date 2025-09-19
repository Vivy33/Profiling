/**
 * @file main.c
 * @brief Linux性能分析工具主程序入口
 * 
 * 该程序是一个基于perf_event和ELF解析的高性能分析工具，能够实时监控系统或特定进程的性能表现。
 * 通过Linux内核的perf_event子系统采集CPU时钟事件，将指令地址解析为具体的函数符号。
 * 
 * 架构流程：
 * 1. 命令行参数解析 → 配置验证 → 系统初始化
 * 2. perf事件初始化 → 事件循环 → 采样处理
 * 3. 地址到符号的完整转换：IP→进程→VMA→ELF→符号
 * 4. 资源清理和优雅退出
 * 
 * 数据结构关系：
 * - system_context: 系统全局上下文，包含进程哈希表和ELF文件缓存
 * - profiling_config: 运行时配置，定义监控策略和采样参数
 * - perf_event_manager: perf事件管理器，封装所有性能事件
 * 
 * 使用示例：
 * 系统级监控：sudo ./my_elf_reader --mode=system --frequency=100
 * 单进程分析：sudo ./my_elf_reader --mode=target --pid=1234
 * 多进程对比：sudo ./my_elf_reader --mode=multi --pids=1001,1002,1003
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libelf.h>

#include "../include/header.h"
#include "../include/config.h"
#include "../include/perf.h"

/**
 * @brief 全局配置对象
 * 
 * 存储程序运行时配置参数，包括：
 * - 监控模式（系统级/单目标/多目标）
 * - 目标进程列表（PID或执行路径）
 * - 采样频率和周期设置
 * - 内核空间过滤选项
 * - 清理间隔和详细输出标志
 * 
 * 该对象在main()中初始化，在整个程序生命周期内有效，
 * 直到程序退出时由free_config()释放。
 */
struct profiling_config global_config;

/**
 * @brief 主程序入口
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return int 程序退出状态码（0=成功，非0=错误）
 * 
 * 程序执行流程：
 * 1. 配置阶段：解析命令行参数，验证配置合法性
 * 2. 初始化阶段：设置perf事件，初始化系统上下文
 * 3. 监控阶段：进入事件循环，处理采样数据
 * 4. 清理阶段：释放所有资源，优雅退出
 * 
 * 错误处理：每个阶段都有完善的错误处理和资源清理机制
 */
int main(int argc, char* argv[]) {
    /**
     * 阶段0: 库初始化
     * 在任何其他操作之前，初始化libelf库
     */
    if (elf_version(EV_CURRENT) == EV_NONE) {
        fprintf(stderr, "Error: Failed to initialize libelf\n");
        return 1;
    }

    /**
     * 阶段1：配置解析
     * 解析命令行参数，填充global_config结构
     * 支持参数：--mode, --pid, --exec, --pids, --execs, --frequency, --filter, --cleanup, --verbose
     */
    if (parse_command_line(argc, argv, &global_config) != 0) {
        fprintf(stderr, "Error: Failed to parse command line arguments\n");
        return 1;
    }

    /**
     * 阶段2：配置验证
     * 验证配置参数的合法性：
     * - 检查目标进程是否存在（MODE_TARGET/MULTI模式）
     * - 验证采样频率范围（必须>0）
     * - 确保模式与参数匹配（如--pid只能在TARGET模式使用）
     */
    if (validate_config(&global_config) != 0) {
        fprintf(stderr, "Error: Configuration validation failed\n");
        return 1;
    }

    /**
     * 阶段4：perf事件初始化
     * 根据配置初始化Linux perf_event：
     * - 系统模式：为每个CPU创建事件，监控所有进程
     * - 目标模式：为每个CPU×目标进程创建事件
     * - 多目标模式：为每个CPU×每个目标进程创建事件
     * 
     * 返回的manager包含所有perf文件描述符和元数据
     */
    struct perf_event_manager* manager = perf_event_init_with_config(&global_config);
    if (!manager) {
        fprintf(stderr, "Error: Failed to initialize perf events.\n");
        fprintf(stderr, "Possible causes: insufficient permissions, perf subsystem disabled\n");
        return 1;
    }

    if (global_config.verbose) {
        printf("Successfully started %d perf events on %d CPUs.\n", 
               manager->num_events, manager->num_cpus);
    }

    /**
     * 阶段5：系统上下文初始化
     * 初始化系统全局上下文，包括：
     * - 进程哈希表：用于PID到进程信息的快速查找
     * - ELF文件缓存：避免重复解析ELF文件
     * - libelf库初始化：确保ELF解析功能可用
     */
    struct system_context system_info;
    if (initialize_system(&system_info)) {
        fprintf(stderr, "Error: Failed to initialize system context\n");
        perf_event_cleanup_manager(manager);
        return 1;
    }

    /**
     * 阶段6：主事件循环
     * 进入性能监控主循环：
     * - 处理每个采样事件，将IP地址解析为符号
     * - 定期清理已终止的进程
     * - 持续运行直到用户中断（Ctrl+C）
     * 
     * 主循环是程序的核心，所有性能数据都在这里收集和处理
     */
    main_loop(&system_info, manager);

    /**
     * 阶段7：资源清理
     * 优雅退出程序，释放所有分配的资源：
     * - 清理进程哈希表和VMA树
     * - 清理ELF文件缓存
     * - 关闭所有perf事件文件描述符
     * - 释放配置内存
     */
    cleanup_system(&system_info);
    perf_event_cleanup_manager(manager);

    return 0;
}
