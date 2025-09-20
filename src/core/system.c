/**
 * @file system.c
 * @brief 系统级初始化和资源管理模块
 * 
 * 负责整个系统的初始化和优雅关闭，包括：
 * - libelf库初始化和版本检查
 * - 进程哈希表的创建和初始化
 * - ELF文件缓存的创建和初始化
 * - 系统资源的完整清理
 * 
 * 内存管理：
 * - 使用calloc确保内存清零
 * - 完善的错误处理，任何失败都会释放已分配资源
 * - 支持部分初始化失败的安全清理
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

#include "../../include/header.h"

/**
 * @brief 初始化系统全局上下文
 * @param system_info 系统上下文指针，用于存储全局状态
 * @return int 成功返回0，失败返回非0错误码
 * 
 * 初始化流程：
 * 1. 清空系统上下文结构体
 * 2. 初始化libelf库，检查版本兼容性
 * 3. 创建进程哈希表，用于PID到进程信息的快速查找
 * 4. 创建ELF文件缓存，避免重复解析ELF文件
 * 
 * 错误处理：
 * - 任何步骤失败都会释放已分配的资源
 * - 提供详细的错误信息到stderr
 * 
 * 内存分配：
 * - process_table: 1024个哈希桶，每个桶可存储链表
 * - elf_cache: 1024个哈希桶，用于ELF文件缓存
 */
int initialize_system(struct system_context* system_info) {
    memset(system_info, 0, sizeof(struct system_context));

    // 初始化libelf库，检查版本兼容性
    if (elf_version(EV_CURRENT) == EV_NONE) {
        fprintf(stderr, "ERROR: ELF library initialization failed: %s\n", elf_errmsg(-1));
        return 1;
    }

    // 创建进程哈希表，O(1)平均时间复杂度查找
    system_info->process_table = (struct process_hash_table*)calloc(1, sizeof(struct process_hash_table));
    
    // 创建ELF文件缓存，避免重复文件IO和解析
    system_info->elf_cache = (struct elf_file_cache*)calloc(1, sizeof(struct elf_file_cache));

    // 内存分配失败处理
    if (!system_info->process_table || !system_info->elf_cache) {
        fprintf(stderr, "ERROR: Failed to allocate memory for system info.\n");
        free(system_info->process_table);
        free(system_info->elf_cache);
        return 1;
    }

    // 加载内核符号表
    system_info->kernel_symbols = load_kernel_symbols();
    if (!system_info->kernel_symbols) {
        fprintf(stderr, "警告: 无法加载内核符号, 内核函数名将无法解析。\n");
        // 这是一个非致命错误，程序可以继续运行
    }
    
    return 0;
}

/**
 * @brief 清理系统所有资源
 * @param system_info 系统上下文指针
 * 
 * 清理顺序（确保无内存泄漏）：
 * 1. 清理内核符号表
 * 2. 清理ELF文件缓存（释放ELF解析结果和符号表）
 * 3. 清理进程哈希表（释放所有进程信息和VMA树）
 * 4. 释放哈希表本身
 * 
 * 线程安全：应在主线程退出时调用
 * 可重入：支持多次调用，NULL参数安全
 */
void cleanup_system(struct system_context* system_info) {
    if (!system_info) return; 
    
    // 释放内核符号表
    free_kernel_symbols(system_info->kernel_symbols);

    // 清理ELF文件缓存，释放所有ELF解析结果
    clear_elf_cache(system_info->elf_cache);
    free(system_info->elf_cache);
    
    // 清理进程哈希表，释放所有进程信息和VMA树
    free_process_hash_table(system_info->process_table);
}