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
#include <string.h>

#include "../include/header.h"
#include "../../include/config.h"
#include "../../include/database.h"

// PERF_CONTEXT_MAX 是有效IP地址的上限。
// 超过此值的地址是上下文标记。此值来自内核UAPI
#ifndef PERF_CONTEXT_MAX
#define PERF_CONTEXT_MAX ((__u64)-4095)
#endif

/**
 * @brief 解析来自perf_event_header的原始采样数据。
 * @param header 指向环形缓冲区中perf_event_header的指针。
 * @param result 指向要填充的callchain_result结构体的指针。
 *
 * 此函数根据配置的sample_type解释头部之后的数据。
 * 它能处理软件调用链和LBR分支栈。
 */
void parse_sample_data(struct perf_event_header *header, struct callchain_result *result, uint64_t max_ips) {
    extern struct profiling_config global_config;
    
    // 安全地初始化结构体成员，而不是使用memset，以保护由调用者设置的ips指针。
    result->pid = 0;
    result->tid = 0;
    result->ip = 0;
    result->nr = 0;

    // 设置安全边界，所有读取都不能超过这个指针
    char *end_ptr = (char *)header + header->size;
    char *ptr = (char *)header + sizeof(struct perf_event_header);

    // 1. 安全地读取 IP
    if (ptr + sizeof(uint64_t) > end_ptr) return;
    result->ip = *(uint64_t *)ptr;
    ptr += sizeof(uint64_t);

    // 2. 安全地读取 PID/TID
    if (ptr + sizeof(uint64_t) > end_ptr) return;
    result->pid = *(uint32_t *)ptr;
    result->tid = *(uint32_t *)(ptr + 4);
    ptr += sizeof(uint64_t);

    // 3. 安全地读取时间戳
    if (ptr + sizeof(uint64_t) > end_ptr) return;
    result->timestamp_ns = *(uint64_t *)ptr;
    ptr += sizeof(uint64_t);

    if (global_config.use_lbr) {
        // LBR 模式: 安全地解析分支栈
        if (ptr + sizeof(uint64_t) > end_ptr) return;
        uint64_t nr_from_data = *(uint64_t *)ptr;
        ptr += sizeof(uint64_t);

        // 根据剩余字节和最大深度，计算实际要复制的分支数量
        uint64_t remaining_bytes = end_ptr - ptr;
        uint64_t nr_from_size = remaining_bytes / sizeof(struct perf_branch_entry);
        uint64_t nr = (nr_from_data < nr_from_size) ? nr_from_data : nr_from_size;
        uint64_t count_to_copy = (nr < max_ips) ? nr : max_ips;

        result->nr = 0;
        struct perf_branch_entry *branches = (struct perf_branch_entry *)ptr;
        for (uint64_t i = 0; i < count_to_copy; i++) {
            // 我们只关心分支的来源地址 'from'，它构成了调用栈
            if (branches[i].from) {
                result->ips[result->nr++] = branches[i].from;
            }
        }
    } else {
        // 软件模式: 安全地解析调用栈
        if (ptr + sizeof(uint64_t) > end_ptr) return;
        uint64_t nr_from_data = *(uint64_t *)ptr;
        ptr += sizeof(uint64_t);

        // 根据记录总大小计算真实的调用栈深度，防止读取垃圾值
        uint64_t remaining_bytes = end_ptr - ptr;
        uint64_t nr_from_size = remaining_bytes / sizeof(uint64_t);

        // 取两个nr中较小的一个，并确保不超过我们自己的缓冲区大小
        uint64_t nr = (nr_from_data < nr_from_size) ? nr_from_data : nr_from_size;
        uint64_t count_to_copy = (nr < max_ips) ? nr : max_ips;
        
        result->nr = count_to_copy;
        if (count_to_copy > 0) {
            memcpy(result->ips, ptr, count_to_copy * sizeof(uint64_t));
        }
    }
}

/**
 * @brief 对完整的调用链进行符号化并打印结果。
 * @param sys 系统上下文，包含进程哈希表和ELF缓存。
 * @param callchain 解析后的调用链数据，包括PID、IP和栈。 
 *
 * 此函数遍历调用链中的每个地址，将其解析为符号（函数名），
 * 并打印符号化的栈回溯。
 */
void symbolize_sample(struct system_context *sys, struct callchain_result *callchain, db_writer_context_t *db_context, uint64_t timestamp_ns) {
    extern struct profiling_config global_config;
    

    // 将主IP和调用链IP合并到一个列表中进行处理。
    uint64_t all_ips[callchain->nr + 2]; // +1 for IP, +1 for safety
    int valid_ips_count = 0;

    // 首先添加主IP，过滤掉内核标记
    if (callchain->ip && callchain->ip < PERF_CONTEXT_MAX) {
        all_ips[valid_ips_count++] = callchain->ip;
    }

    // 添加调用链IP，过滤掉内核标记
    for (uint64_t i = 0; i < callchain->nr; i++) {
        if (callchain->ips[i] < PERF_CONTEXT_MAX) {
            all_ips[valid_ips_count++] = callchain->ips[i];
        }
    }

    // 获取进程信息用于构建火焰图
    struct process_info* proc_info = find_new_process(sys->process_table, callchain->pid);
    const char* process_name = (proc_info && proc_info->process_name) ? proc_info->process_name : "unknown";

    // 构建火焰图兼容的调用链
    char flame_buffer[16384] = {0};
    int flame_len = 0;

    // 火焰图格式：进程名;函数1;函数2;函数3
    // 首先添加进程名称
    flame_len += snprintf(flame_buffer + flame_len, sizeof(flame_buffer) - flame_len, "%s", process_name);

    // 对链中的每个地址进行符号化，构建调用链
    for (int i = 0; i < valid_ips_count; i++) {
        uint64_t current_ip = all_ips[i];

        // 黑魔法
        bool is_kernel_addr = (current_ip & 0x8000000000000000ULL) != 0;

        // 地址空间过滤
        switch (global_config.filter_mode) {
            case FILTER_USER: if (is_kernel_addr) continue; break;
            case FILTER_KERNEL: if (!is_kernel_addr) continue; break;
            default: break;
        }

        // 构建函数名
        char func_name[512] = {0};
        if (is_kernel_addr) {
            const char *kernel_symbol = find_kernel_symbol(sys->kernel_symbols, current_ip);
            snprintf(func_name, sizeof(func_name), "%s [kernel]", kernel_symbol);
        } else {
            if (!proc_info) {
                snprintf(func_name, sizeof(func_name), "[unknown_process]");
            } else {
                // 通过比较进程启动时间来检测PID复用
                unsigned long long current_start_time = get_process_start_time(proc_info->process_id);
                if (proc_info->start_time != current_start_time) {
                    remove_process(sys, proc_info->process_id);
                    proc_info = find_new_process(sys->process_table, callchain->pid);
                    if (!proc_info) {
                        snprintf(func_name, sizeof(func_name), "[process_recycled]");
                    }
                }

                if (proc_info && proc_info->memory_map_tree.rb_node != NULL) {
                    struct virtual_memory_area* vma_info = find_vma_from_process(proc_info, current_ip);
                    if (!vma_info) {
                        snprintf(func_name, sizeof(func_name), "[unknown_vma]");
                    } else if (!vma_info->mapping_name || vma_info->mapping_name[0] == '\0' || vma_info->mapping_name[0] == '[') {
                        const char* area_name = vma_info->mapping_name ? vma_info->mapping_name : "[anonymous]";
                        snprintf(func_name, sizeof(func_name), "%s", area_name);
                    } else {
                        if (!vma_info->elf_file) {
                            vma_info->elf_file = find_or_create_elf(sys, callchain->pid, vma_info->mapping_name);
                        }

                        struct elf_file* elf = vma_info->elf_file;
                        if (!elf) {
                            snprintf(func_name, sizeof(func_name), "[unknown_elf:%s]", vma_info->mapping_name);
                        } else {
                            uint64_t rel_addr = get_relative_address(current_ip, vma_info);
                            const char* symbol_name = find_symbol_name_from_elf(elf, rel_addr);
                            if (strlen(symbol_name) > 0) {
                                snprintf(func_name, sizeof(func_name), "%s", symbol_name);
                            } else {
                                snprintf(func_name, sizeof(func_name), "[unknown_symbol:%s]", elf->file_path);
                            }
                        }
                    }
                } else {
                    snprintf(func_name, sizeof(func_name), "[vma_unavailable]");
                }
            }
        }

        // 添加到调用链
        if (flame_len + strlen(func_name) + 2 < sizeof(flame_buffer)) {
            if (flame_len > strlen(process_name)) {
                strcat(flame_buffer, ";");
            }
            strcat(flame_buffer, func_name);
        }
    }

    // 构建显示用的栈信息（用于调试和显示）
    char display_buffer[16384] = {0};
    int display_len = 0;
    for (int i = 0; i < valid_ips_count; i++) {
        uint64_t current_ip = all_ips[i];

        for (int j = 0; j < i; j++) {
            display_len += snprintf(display_buffer + display_len, sizeof(display_buffer) - display_len, "  ");
        }

        bool is_kernel_addr = (current_ip & 0x8000000000000000ULL) != 0;
        if (is_kernel_addr) {
            const char *kernel_symbol = find_kernel_symbol(sys->kernel_symbols, current_ip);
            display_len += snprintf(display_buffer + display_len, sizeof(display_buffer) - display_len,
                                    "%s [kernel]\n", kernel_symbol);
        } else {
            display_len += snprintf(display_buffer + display_len, sizeof(display_buffer) - display_len,
                                    "%s\n", proc_info ? "[user_function]" : "[unknown_process]");
        }
    }

    if (valid_ips_count > 0) {
        // 使用火焰图兼容格式：进程名;函数1;函数2;函数3
        char flame_final[16384 + 256];
        snprintf(flame_final, sizeof(flame_final), "%s", flame_buffer);

        // 同时存储显示格式：pid|process_name|display_stack
        char display_final[16384 + 256];
        snprintf(display_final, sizeof(display_final), "%d|%s|%s",
                 callchain->pid, process_name, display_buffer);

        uint64_t realtime_timestamp_ns = callchain->timestamp_ns;
        if (sys->monotonic_start_ns != 0 && sys->realtime_start_ns != 0) {
            realtime_timestamp_ns = callchain->timestamp_ns - sys->monotonic_start_ns + sys->realtime_start_ns;
        }

        // 存储两种格式：火焰图格式用于火焰图工具，显示格式用于查询
        db_writer_push_stack(db_context, realtime_timestamp_ns, flame_final);
    } else {
        // 即使没有有效IP，也记录火焰图格式
        char basic_flame[512];
        snprintf(basic_flame, sizeof(basic_flame), "%s", process_name);

        uint64_t realtime_timestamp_ns = callchain->timestamp_ns;
        if (sys->monotonic_start_ns != 0 && sys->realtime_start_ns != 0) {
            realtime_timestamp_ns = callchain->timestamp_ns - sys->monotonic_start_ns + sys->realtime_start_ns;
        }
        db_writer_push_stack(db_context, realtime_timestamp_ns, basic_flame);
    }
}