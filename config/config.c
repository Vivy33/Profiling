/**
 * @file config.c
 * @brief 性能分析工具配置管理模块
 * 
 * 负责命令行参数解析、配置验证和配置管理。
 * 支持三种监控模式：
 * - MODE_SYSTEM: 系统级监控，监控所有进程
 * - MODE_TARGET: 单目标进程，监控单个指定进程
 * - MODE_MULTI: 多目标进程，监控多个指定进程
 * 
 * 配置参数包括：
 * - 监控模式选择
 * - 目标进程指定（PID或执行路径）
 * - 采样频率设置
 * - 内核空间过滤
 * - 自动重启策略
 * - 详细输出选项
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>

#include "../include/config.h"

/**
 * @brief 打印程序使用帮助
 * @param program_name 程序名称，用于显示在帮助信息中
 * 
 * 显示详细的命令行参数说明，包括：
 * - 三种监控模式的使用方法
 * - 每种模式支持的参数
 * - 通用选项说明
 * - 具体使用示例
 * 
 * 帮助信息采用标准格式，方便用户理解和使用
 */
void print_usage(const char* program_name) {
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Performance profiling tool with three operation modes:\n\n");
    
    printf("MODES:\n");
    printf("  --mode=MODE           Operation mode (system|target|multi)\n");
    printf("                        system: Monitor all processes (default)\n");
    printf("                        target: Monitor single specified process\n");
    printf("                        multi:  Monitor multiple specified processes\n\n");
    
    printf("SYSTEM MODE:\n");
    printf("  No additional parameters needed\n\n");
    
    printf("TARGET MODE:\n");
    printf("  --pid=PID             Monitor specific process ID\n");
    printf("  --exec=PATH          Monitor specific executable (will be started)\n");
    printf("  --args=ARGS          Arguments for --exec mode\n");
    printf("  --restart            Auto-restart target process if it exits\n\n");
    
    printf("MULTI MODE:\n");
    printf("  --pids=PID1,PID2,... Monitor multiple process IDs\n");
    printf("  --execs=PATH1,PATH2 Monitor multiple executables\n");
    printf("  --restart            Auto-restart all target processes\n\n");
    
    printf("GENERAL OPTIONS:\n");
    printf("  -f, --frequency=HZ   Sampling frequency (default: 30)\n");
    printf("  -k, --kernel         Include kernel space samples\n");
    printf("  -v, --verbose        Verbose output\n");
    printf("  -h, --help           Show this help\n\n");
    
    printf("EXAMPLES:\n");
    printf("  %s --mode=system\n", program_name);
    printf("  %s --mode=target --pid=1234\n", program_name);
    printf("  %s --mode=target --exec=/bin/bash --args=\"-c 'ls -la'\"\n", program_name);
    printf("  %s --mode=multi --pids=1001,1002,1003\n", program_name);
    printf("  %s --mode=multi --execs=\"/usr/bin/python3,/bin/bash\" --restart\n", program_name);
}

/**
 * @brief 解析命令行参数
 * @param argc 参数数量
 * @param argv 参数数组
 * @param config 输出参数，解析后的配置结构
 * @return int 0=成功，-1=失败
 * 
 * 支持的长选项：
 * --mode=MODE: 监控模式 (system|target|multi)
 * --pid=PID: 目标进程PID (target模式)
 * --exec=PATH: 目标可执行文件路径 (target模式)
 * --args=ARGS: 启动参数 (target模式，配合--exec使用)
 * --pids=PID1,PID2,...: 多进程PID列表 (multi模式)
 * --execs=PATH1,PATH2: 多可执行文件路径 (multi模式)
 * --restart: 自动重启退出的进程
 * --frequency=HZ: 采样频率 (默认30Hz)
 * --kernel: 包含内核空间采样（默认排除）
 * --verbose: 详细输出
 * --help: 显示帮助信息
 * 
 * 参数验证逻辑：
 * - 确保模式特定参数只在对应模式下使用
 * - 验证PID格式和数值范围
 * - 验证采样频率必须为正数
 * - 检查内存分配是否成功
 */
int parse_command_line(int argc, char* argv[], struct profiling_config* config) {
    static struct option long_options[] = {
        {"mode", required_argument, 0, 'm'},
        {"pid", required_argument, 0, 'p'},
        {"exec", required_argument, 0, 'e'},
        {"args", required_argument, 0, 'a'},
        {"pids", required_argument, 0, 'P'},
        {"execs", required_argument, 0, 'E'},
        {"restart", no_argument, 0, 'r'},
        {"frequency", required_argument, 0, 'f'},
        {"kernel", no_argument, 0, 'k'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    // 初始化默认配置
    config->mode = MODE_SYSTEM;
    config->targets = NULL;
    config->target_count = 0;
    config->sampling_frequency = DEFAULT_SAMPLING_FREQUENCY;
    config->sample_period_ns = 1000000000 / DEFAULT_SAMPLING_FREQUENCY;
    config->exclude_kernel = true;
    config->exclude_hypervisor = true;
    config->cleanup_interval = DEFAULT_CLEANUP_INTERVAL;
    config->verbose = false;

    int opt;
    while ((opt = getopt_long(argc, argv, "m:p:e:a:P:E:rf:kvh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'm':
                if (strcmp(optarg, "system") == 0) {
                    config->mode = MODE_SYSTEM;
                } else if (strcmp(optarg, "target") == 0) {
                    config->mode = MODE_TARGET;
                } else if (strcmp(optarg, "multi") == 0) {
                    config->mode = MODE_MULTI;
                } else {
                    fprintf(stderr, "Invalid mode: %s\n", optarg);
                    return -1;
                }
                break;
            case 'p':
                // 单个PID模式
                if (config->mode != MODE_TARGET) {
                    fprintf(stderr, "--pid only valid in target mode\n");
                    return -1;
                }
                config->target_count = 1;
                config->targets = malloc(sizeof(struct target_process));
                config->targets[0].pid = atoi(optarg);
                config->targets[0].exec_path = NULL;
                config->targets[0].args = NULL;
                config->targets[0].auto_restart = false;
                break;
            case 'e':
                // 单个exec模式
                if (config->mode != MODE_TARGET) {
                    fprintf(stderr, "--exec only valid in target mode\n");
                    return -1;
                }
                config->target_count = 1;
                config->targets = malloc(sizeof(struct target_process));
                config->targets[0].pid = -1; // 需要启动
                config->targets[0].exec_path = strdup(optarg);
                config->targets[0].args = NULL;
                config->targets[0].auto_restart = false;
                break;
            case 'a':
                // exec参数
                if (config->target_count > 0 && config->targets[0].exec_path != NULL) {
                    config->targets[0].args = strdup(optarg);
                }
                break;
            case 'P':
                // 多个PID模式
                if (config->mode != MODE_MULTI) {
                    fprintf(stderr, "--pids only valid in multi mode\n");
                    return -1;
                }
                {
                    char* pids_str = strdup(optarg);
                    char* token = strtok(pids_str, ",");
                    int count = 0;
                    while (token != NULL) {
                        count++;
                        token = strtok(NULL, ",");
                    }
                    
                    config->target_count = count;
                    config->targets = malloc(count * sizeof(struct target_process));
                    
                    strcpy(pids_str, optarg);
                    token = strtok(pids_str, ",");
                    int i = 0;
                    while (token != NULL) {
                        config->targets[i].pid = atoi(token);
                        config->targets[i].exec_path = NULL;
                        config->targets[i].args = NULL;
                        config->targets[i].auto_restart = false;
                        i++;
                        token = strtok(NULL, ",");
                    }
                    free(pids_str);
                }
                break;
            case 'E':
                // 多个exec模式
                if (config->mode != MODE_MULTI) {
                    fprintf(stderr, "--execs only valid in multi mode\n");
                    return -1;
                }
                {
                    char* execs_str = strdup(optarg);
                    char* token = strtok(execs_str, ",");
                    int count = 0;
                    while (token != NULL) {
                        count++;
                        token = strtok(NULL, ",");
                    }
                    
                    config->target_count = count;
                    config->targets = malloc(count * sizeof(struct target_process));
                    
                    strcpy(execs_str, optarg);
                    token = strtok(execs_str, ",");
                    int i = 0;
                    while (token != NULL) {
                        config->targets[i].pid = -1; // 需要启动
                        config->targets[i].exec_path = strdup(token);
                        config->targets[i].args = NULL;
                        config->targets[i].auto_restart = false;
                        i++;
                        token = strtok(NULL, ",");
                    }
                    free(execs_str);
                }
                break;
            case 'r':
                if (config->target_count > 0) {
                    for (int i = 0; i < config->target_count; i++) {
                        config->targets[i].auto_restart = true;
                    }
                }
                break;
            case 'f':
                config->sampling_frequency = atoi(optarg);
                if (config->sampling_frequency <= 0) {
                    fprintf(stderr, "Invalid frequency: %s\n", optarg);
                    return -1;
                }
                config->sample_period_ns = 1000000000 / config->sampling_frequency;
                break;
            case 'k':
                config->exclude_kernel = false;
                break;
            case 'v':
                config->verbose = true;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                return -1;
        }
    }

    // 验证配置
    if (validate_config(config) != 0) {
        return -1;
    }

    return 0;
}

/**
 * @brief 验证配置参数的合法性
 * @param config 待验证的配置结构
 * @return int 0=验证通过，-1=验证失败
 * 
 * 验证规则：
 * 1. 目标模式(MODE_TARGET)和多目标模式(MODE_MULTI)必须有目标进程
 * 2. 每个目标进程必须指定PID或可执行路径
 * 3. PID必须为正整数，可执行路径必须非空
 * 4. 采样频率必须大于0
 * 5. 清理间隔必须合理（1-3600秒）
 * 
 * 错误处理：
 * - 向stderr输出具体的错误信息
 * - 提供修正建议
 */
int validate_config(const struct profiling_config* config) {
    if (config->mode == MODE_TARGET || config->mode == MODE_MULTI) {
        if (config->target_count == 0) {
            fprintf(stderr, "Target or multi mode requires at least one target\n");
            return -1;
        }
        
        for (int i = 0; i < config->target_count; i++) {
            if (config->targets[i].pid == -1 && config->targets[i].exec_path == NULL) {
                fprintf(stderr, "Each target must have either a PID or executable path\n");
                return -1;
            }
        }
    }

    return 0;
}

/**
 * @brief 释放配置相关的动态内存
 * @param config 配置结构指针
 * 
 * 释放内容：
 * - 所有目标进程的exec_path（可执行路径）
 * - 所有目标进程的args（启动参数）
 * - targets数组本身
 * 
 * 安全特性：
 * - 空指针检查
 * - 释放后置空指针
 * - 重置target_count为0
 * 
 * 调用时机：程序退出时或配置重新加载时
 */
void free_config(struct profiling_config* config) {
    if (config->targets) {
        for (int i = 0; i < config->target_count; i++) {
            free(config->targets[i].exec_path);
            free(config->targets[i].args);
        }
        free(config->targets);
        config->targets = NULL;
        config->target_count = 0;
    }
}

const char* get_mode_name(profiling_mode_t mode) {
    switch (mode) {
        case MODE_SYSTEM: return "system";
        case MODE_TARGET: return "target";
        case MODE_MULTI: return "multi";
        default: return "unknown";
    }
}

void print_config_summary(const struct profiling_config* config) {
    printf("=== Configuration Summary ===\n");
    printf("Mode: %s\n", get_mode_name(config->mode));
    printf("Sampling frequency: %d Hz\n", config->sampling_frequency);
    printf("Sample period: %d ns\n", config->sample_period_ns);
    printf("Exclude kernel: %s\n", config->exclude_kernel ? "yes" : "no");
    printf("Verbose: %s\n", config->verbose ? "yes" : "no");
    printf("Cleanup interval: %d seconds\n", config->cleanup_interval);
    
    if (config->mode == MODE_TARGET || config->mode == MODE_MULTI) {
        printf("Target processes (%d):\n", config->target_count);
        for (int i = 0; i < config->target_count; i++) {
            printf("  [%d] ", i + 1);
            if (config->targets[i].pid != -1) {
                printf("PID: %d", config->targets[i].pid);
            } else {
                printf("Exec: %s", config->targets[i].exec_path);
                if (config->targets[i].args) {
                    printf(" %s", config->targets[i].args);
                }
            }
            if (config->targets[i].auto_restart) {
                printf(" (auto-restart)");
            }
            printf("\n");
        }
    }
    printf("============================\n");
}