/**
 * @file handler.c
 * @brief 采样数据处理核心模块
 * 
 * 负责将perf_event产生的原始采样数据转换为可读的性能信息。
 * 实现了完整的地址到符号的转换流程：
 * 
 * 数据流：
 * sample_data → 进程查找 → VMA定位 → ELF解析 → 符号名称
 * 
 * 关键技术：
 * - 进程过滤：根据配置只处理目标进程
 * - VMA树查找：使用红黑树快速定位地址所属内存区域
 * - ELF缓存：避免重复解析同一ELF文件
 * - 符号解析：将运行时地址转换为函数名
 * 
 * 过滤策略：
 * - 系统模式：处理所有进程
 * - 目标模式：只处理指定PID的进程
 * - 多目标模式：只处理指定PID列表中的进程
 */

#include <stdio.h>
#include <stdbool.h>

#include "../../include/header.h"
#include "../../include/config.h"

/**
 * @brief 处理单个perf采样事件
 * @param sys 系统上下文，包含进程哈希表和ELF缓存
 * @param data 采样数据，包含PID、指令地址等关键信息
 * 
 * 处理流程：
 * 1. 根据配置模式过滤进程
 * 2. 查找或创建进程信息
 * 3. 查找地址对应的虚拟内存区域(VMA)
 * 4. 计算相对文件偏移
 * 5. 查找或解析ELF文件
 * 6. 查找符号名称并输出
 * 
 * 输出格式：
 * 详细模式：PID: 1234, IP: 0x7f8b3c45a280, Symbol: malloc in /lib/libc.so.6
 * 简洁模式：1234: malloc
 */
void handle_sample(struct system_context *sys, struct sample_data *data) {
    extern struct profiling_config global_config;
    
    /**
     * 步骤1：进程过滤
     * 根据监控模式决定是否处理该进程：
     * - 系统模式：处理所有进程
     * - 目标模式：只处理指定PID的进程
     * - 多目标模式：只处理指定PID列表中的进程
     * 
     * 过滤逻辑：遍历目标进程列表，检查当前PID是否匹配
     */
    if (global_config.mode == MODE_TARGET || global_config.mode == MODE_MULTI) {
        bool is_target = false;
        for (int i = 0; i < global_config.target_count; i++) {
            if (data->pid == global_config.targets[i].pid) {
                is_target = true;
                break;
            }
        }
        if (!is_target) return;
    }

    /**
     * 步骤2：进程查找或创建
     * 根据PID查找进程信息，如果进程不存在则创建：
     * - 在进程哈希表中查找
     * - 如果未找到，创建新的进程信息并解析内存映射
     * - 内存映射包括所有VMA区域，用于后续地址查找
     */
    struct process_info* proc_info = find_new_process(sys->process_table, data->pid);
    if (!proc_info) {
        // 进程创建失败（通常是由于内存不足或权限问题）
        return;
    }

    /**
     * 步骤3：VMA查找
     * 在给定进程的VMA红黑树中查找包含指令地址的内存区域：
     * - 使用红黑树实现O(log n)查找
     * - VMA包含起始地址、结束地址、文件映射信息
     * - 如果地址不在任何VMA范围内，返回NULL
     */
    struct virtual_memory_area* vma_info = find_vma_from_process(proc_info, data->ip);
    if (!vma_info) {
        // 地址不在任何已知的VMA范围内，可能是匿名内存或新创建的映射
        return;
    }

    /**
     * 步骤4：计算相对地址
     * 将运行时地址转换为ELF文件中的相对偏移：
     * 相对地址 = (运行时地址 - VMA起始地址) + 文件偏移
     * 
     * 这个转换是必要的，因为运行时地址是虚拟地址，
     * 而ELF符号表中的地址是相对于文件开头的偏移
     */
    uint64_t rel_addr = get_relative_address(data->ip, vma_info);
    
    /**
     * 步骤5：ELF文件查找或解析
     * 根据VMA对应的文件名获取ELF文件信息：
     * - 先在ELF缓存中查找
     * - 如果未找到，解析ELF文件并创建缓存
     * - 使用引用计数管理缓存生命周期
     */
    struct elf_file* elf = find_or_create_elf(sys, vma_info->mapping_name);
    if (!elf) {
        // ELF文件解析失败（可能文件不存在或格式错误）
        return;
    }
    
    /**
     * 步骤6：符号查找
     * 在ELF文件的符号表中查找相对地址对应的函数名称：
     * - 使用红黑树实现O(log n)符号查找
     * - 支持函数符号、对象符号等多种类型
     * - 如果未找到符号，返回"unknown_function"
     */
    const char* symbol_name = find_symbol_name_from_elf(elf, rel_addr);
    
    /**
     * 步骤7：结果输出
     * 根据详细输出标志决定输出格式：
     * 详细模式：包含PID、地址、符号名、ELF文件路径
     * 简洁模式：只显示PID和符号名
     */
    if (global_config.verbose) {
        printf("PID: %d, IP: 0x%lx, Symbol: %s in %s\n", 
               data->pid, data->ip, symbol_name, elf->file_path);
    } else {
        printf("%d: %s\n", data->pid, symbol_name);
    }
}
