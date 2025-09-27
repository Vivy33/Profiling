/*
 * 命令行参数配置系统 - 中文说明
 *
 * 本文件定义了所有可配置的命令行参数，用于灵活控制性能分析行为：
 *
 * 【采样频率控制】
 * --frequency=NUM      设置采样频率(Hz)，默认30Hz，范围1-10000
 *
 * 【分析过滤】
 * --filter=MODE        显示模式: user|kernel|all (默认: all)
 *
 * 【系统管理】
 * --cleanup=SEC        清理的是内存中的数据结构，间隔秒数，默认30秒
                        - 进程/线程元数据 - 已退出进程的跟踪信息
                        - 缓存条目 - 死进程的VMA、符号、ELF缓存
                        - 红黑树节点 - 进程树中已退出的节点
                        - 内存映射 - 死进程的内存映射信息
 * --help               显示帮助信息
 *
 * 【使用示例】
 * ./profiling_tool --frequency=50 --filter=user --cleanup=2
 * ./profiling_tool --help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/config.h"

/**
 * @brief 打印程序使用说明
 * @param program_name 程序名称
 */
void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  --frequency=NUM    Sampling frequency in Hz (default: %d)\n", DEFAULT_SAMPLING_FREQUENCY);
    printf("  --filter=MODE      Display filter: user|kernel|all (default: all)\n");
    printf("  --cleanup=SEC      Cleanup interval in seconds (default: %d)\n", DEFAULT_CLEANUP_INTERVAL);
    printf("  --stack-depth=NUM  Set max stack backtrace depth (default: %d)\n", DEFAULT_MAX_STACK_DEPTH);
    printf("  --output-dir=PATH  Directory for SQLite database storage (default: /tmp)\n");
    printf("  --http-port=NUM    HTTP server listening port (default: %d)\n", DEFAULT_HTTP_PORT);
    printf("  --log-dir=PATH     Directory for program logs (default: /log)\n");
    printf("  --lbr              Enable Last Branch Record (LBR) for precise call stacks\n");
    printf("  --help             Show this help message\n");
}

/**
 * @brief 解析命令行参数
 * @param argc 参数数量
 * @param argv 参数数组
 * @param config 配置结构体指针
 * @return 0表示解析成功，非0表示解析失败
 */
int parse_command_line(int argc, char* argv[], struct profiling_config* config) {
    // 设置默认配置
    config->sampling_frequency = DEFAULT_SAMPLING_FREQUENCY;
    config->filter_mode = FILTER_ALL;
    config->cleanup_interval = DEFAULT_CLEANUP_INTERVAL;
    config->max_stack_depth = DEFAULT_MAX_STACK_DEPTH;
    config->use_lbr = false;
    config->db_output_dir = DEFAULT_DB_DIR;
    config->http_port = DEFAULT_HTTP_PORT; // 设置默认HTTP端口
    config->log_output_dir = DEFAULT_LOG_DIR;
    config->histogram_print_threshold = DEFAULT_HISTOGRAM_PRINT_THRESHOLD;
    config->db_batch_size = 40; // Default batch size
    
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--frequency=", 12) == 0) {
            fprintf(stderr, "DEBUG: Received frequency argument: %s\n", argv[i] + 12);
            config->sampling_frequency = atoi(argv[i] + 12);
            if (config->sampling_frequency <= 0) {
                fprintf(stderr, "Error: Invalid sampling frequency\n");
                return -1;
            }
        } else if (strncmp(argv[i], "--filter=", 9) == 0) {
            const char* mode = argv[i] + 9;
            if (strcmp(mode, "user") == 0) {
                config->filter_mode = FILTER_USER;
            } else if (strcmp(mode, "kernel") == 0) {
                config->filter_mode = FILTER_KERNEL;
            } else if (strcmp(mode, "all") == 0) {
                config->filter_mode = FILTER_ALL;
            } else {
                fprintf(stderr, "Error: Invalid filter mode '%s'. Use user|kernel|all\n", mode);
                return -1;
            }
        } else if (strncmp(argv[i], "--stack-depth=", 14) == 0) {
            config->max_stack_depth = atoi(argv[i] + 14);
            if (config->max_stack_depth <= 0) {
                fprintf(stderr, "Error: Invalid stack depth\n");
                return -1;
            }
        } else if (strncmp(argv[i], "--cleanup=", 10) == 0) {
            config->cleanup_interval = atoi(argv[i] + 10);
            if (config->cleanup_interval <= 0) {
                fprintf(stderr, "Error: Invalid cleanup interval\n");
                return -1;
            }
        } else if (strncmp(argv[i], "--output-dir=", 13) == 0) {
            config->db_output_dir = argv[i] + 13;
        } else if (strncmp(argv[i], "--http-port=", 12) == 0) {
            config->http_port = atoi(argv[i] + 12);
            if (config->http_port <= 0 || config->http_port > 65535) {
                fprintf(stderr, "Error: Invalid HTTP port\n");
                return -1;
            }
        } else if (strncmp(argv[i], "--log-dir=", 10) == 0) {
            config->log_output_dir = strdup(argv[i] + 10);
            if (!config->log_output_dir) {
                fprintf(stderr, "Error: Failed to allocate memory for log_output_dir\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--lbr") == 0) {
            config->use_lbr = true;
        } else if (strncmp(argv[i], "--histogram-threshold=", 24) == 0) {
            config->histogram_print_threshold = atoi(argv[i] + 24);
            if (config->histogram_print_threshold <= 0) {
                fprintf(stderr, "Error: Invalid histogram print threshold\n");
                return -1;
            }
        } else if (strncmp(argv[i], "--db-batch-size=", 16) == 0) {
            config->db_batch_size = atoi(argv[i] + 16);
            if (config->db_batch_size <= 0) {
                fprintf(stderr, "Error: Invalid db batch size\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }
    
    return 0;
}

/**
 * @brief 验证配置参数
 * @param config 配置结构体指针
 * @return 0表示验证通过，非0表示验证失败
 * 
 * 系统级性能分析配置验证：
 * - 验证采样频率必须大于0
 * - 验证其他基本参数合法性
 */
int validate_config(struct profiling_config* config) {
    if (!config) {
        fprintf(stderr, "Error: NULL config pointer\n");
        return -1;
    }
    
    if (config->sampling_frequency <= 0) {
        fprintf(stderr, "Error: Invalid sampling frequency: %d\n", config->sampling_frequency);
        return -1;
    }
    
    if (config->cleanup_interval <= 0) {
        fprintf(stderr, "Error: Invalid cleanup interval: %d\n", config->cleanup_interval);
        return -1;
    }
    
    if (config->http_port <= 0 || config->http_port > 65535) {
        fprintf(stderr, "Error: Invalid HTTP port: %d\n", config->http_port);
        return -1;
    }

    return 0;
}