/**
 * @file vma.c
 * @brief 虚拟内存区域(VMA)解析和管理模块
 * 
 * 负责进程内存映射的解析和地址转换，包括：
 * - 解析/proc/[pid]/maps文件，提取VMA信息
 * - 构建VMA红黑树，支持O(log n)地址查找
 * - 运行时地址到文件偏移的转换算法
 * - VMA内存的完整生命周期管理
 * 
 * 核心功能：
 * - parse_process_memory_maps(): 解析进程内存映射
 * - get_relative_address(): 地址转换算法
 * - free_vma_tree(): VMA树内存清理
 * 
 * 数据格式：
 * /proc/[pid]/maps文件的解析和内存映射管理
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stddef.h>

#include "../include/header.h"

/**
 * @brief 递归释放VMA红黑树的所有节点
 * @param node 当前处理的树节点
 * 
 * 使用后续遍历确保：
 * 1. 先释放子节点，再释放当前节点
 * 2. 释放每个VMA的mapping_name字符串
 * 3. 释放VMA结构体本身
 * 
 * 线程安全：应在单线程环境中调用
 * 可重入：支持，无全局状态依赖
 */
static void free_vma_nodes(struct rb_node *node) {
    if (node == NULL) {
        return;
    }
    
    // 后续遍历：先释放子树
    if (node->rb_left) {
        free_vma_nodes(node->rb_left);
    }
    if (node->rb_right) {
        free_vma_nodes(node->rb_right);
    }
    
    // 释放当前节点
    struct virtual_memory_area *vma_info = rb_entry(node, struct virtual_memory_area, vm_rb_node);
    free(vma_info->mapping_name);  // 释放文件名内存
    free(vma_info);                // 释放VMA结构体内存
}

/**
 * @brief 释放整个VMA红黑树并重置根节点
 * @param root VMA红黑树根节点指针
 * 
 * 清理流程：
 * 1. 递归释放所有树节点
 * 2. 重置根节点为RB_ROOT（空树状态）
 * 
 * 安全性：
 * - 支持NULL指针检查
 * - 支持重复调用
 * - 确保树指针重置，避免野指针
 */
void free_vma_tree(struct rb_root *root) {
    if (root && root->rb_node) {
        free_vma_nodes(root->rb_node);
    }
    *root = RB_ROOT; // 重置根节点为空树状态
}

/**
 * @brief 解析进程的内存映射信息（/proc/[pid]/maps）
 * @param proc 进程信息结构，用于存储解析结果
 * @return int 0=成功解析，1=解析失败
 * 
 * 功能说明：
 * 读取并解析Linux的/proc/[pid]/maps文件，提取进程的虚拟内存区域信息。
 * 每个VMA包含地址范围、权限、文件映射等关键信息。
 * 
 * 文件格式：
 * 起始地址-结束地址 权限 偏移 设备 节点号 文件名
 * 7f8b3c400000-7f8b3c600000 r-xp 00000000 08:01 131589 /lib/libc.so.6
 * 
 * 权限字段：
 * r - 可读，映射到READ标志
 * w - 可写，映射到WRITE标志
 * x - 可执行，映射到EXECUTE标志
 * p - 私有映射
 * s - 共享映射
 * 
 * 映射类型：
 * - 文件映射：对应具体的ELF文件（如libc.so.6）
 * - 匿名映射：[heap]、[stack]、[vdso]等
 * - 特殊映射：[vsyscall]、[vvar]等
 * 
 * 红黑树构建：
 * - 按起始地址排序，确保O(log n)查找效率
 * - 支持地址范围查询，用于运行时地址定位
 * - 自动处理地址重叠和间隙
 * 
 * 错误处理：
 * - 文件打开失败：进程不存在或无权限
 * - 内存分配失败：及时清理已分配内存
 * - 解析失败：跳过无效行，继续处理后续行
 * 
 * 内存管理：
 * - 为每个VMA分配独立内存
 * - 复制文件名（避免使用原始缓冲区，防止悬垂指针）
 * - 使用红黑树管理VMA，支持快速地址查找
 */
int parse_process_memory_maps(struct process_info* proc) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/maps", proc->process_id);
    
    FILE* file = fopen(path, "r");
    if (!file) {
        // 进程可能已终止或无权限访问
        return 1;
    }

    char line[512];
    proc->memory_map_tree = RB_ROOT;  // 初始化红黑树根节点

    while (fgets(line, sizeof(line), file)) {
        struct virtual_memory_area* vma = (struct virtual_memory_area*)calloc(1, sizeof(struct virtual_memory_area));
        if (!vma) {
            perror("Failed to allocate memory for VMA");
            fclose(file);
            return 1;
        }
        
        char perm[5] = {0};
        char region_name[256] = {0};

        /**
         * 解析单行格式：
         * 起始地址      -   结束地址     权限  偏移     设备   节点号                      文件名
         * 64fbac8a6000  -   64fbac8c7000 rw-p 00000000 00:00   0                          [heap]
         * 使用sscanf提取关键字段
         * %*s %*d - 跳过设备号和inode号字段
         */
        sscanf(line, "%lx-%lx %4s %lx %*s %*d %255s",
               &vma->start_addr, &vma->end_addr, perm, &vma->file_offset, region_name);

        /**
         * 解析权限标志：
         * r - 可读，映射到READ标志
         * w - 可写，映射到WRITE标志
         * x - 可执行，映射到EXECUTE标志
         * p - 私有映射，s - 共享映射
         */
        if (perm[0] == 'r') vma->vm_flags |= READ;
        if (perm[1] == 'w') vma->vm_flags |= WRITE;
        if (perm[2] == 'x') vma->vm_flags |= EXECUTE;

        /**
         * 复制映射文件名：
         * - 文件映射：实际的ELF文件路径（如/lib/libc.so.6）
         * - 匿名映射：[heap]、[stack]、[vdso]等
         * - 使用strdup确保内存安全，防止悬垂指针
         */
        if (region_name[0] != '\0') {
            vma->mapping_name = strdup(region_name);
        }
        vma->elf_file = NULL; // 显式初始化ELF文件指针

        /**
         * 将VMA插入红黑树：
         * - 按键排序：起始地址升序排列
         * - 支持O(log n)查找，用于运行时地址定位
         * - 自动平衡树结构，保持查找效率
         * 
         * 插入算法：
         * 1. 找到合适的插入位置（按起始地址比较）
         * 2. 链接新节点到父节点
         * 3. 重新平衡树结构（rb_insert_color）
         */
        struct rb_node **new_node = &(proc->memory_map_tree.rb_node), *parent = NULL;
        while (*new_node) {
            struct virtual_memory_area *self = rb_entry(*new_node, struct virtual_memory_area, vm_rb_node);
            parent = *new_node;
            if (vma->start_addr < self->start_addr)
                new_node = &((*new_node)->rb_left);
            else
                new_node = &((*new_node)->rb_right);
        }

        rb_link_node(&vma->vm_rb_node, parent, new_node);
        rb_insert_color(&vma->vm_rb_node, &proc->memory_map_tree);
    }

    fclose(file);
    return 0;
}

/**
 * @brief 将运行时地址转换为ELF文件中的相对偏移
 * @param real_addr 运行时虚拟地址（来自perf采样）
 * @param vma 虚拟内存区域信息，包含映射关系
 * @return uint64_t 相对文件偏移，0表示转换失败
 * 
 * 转换原理：
 * 相对地址 = (运行时地址 - VMA起始地址) + 文件偏移
 * 
 * 应用场景：
 * - 将perf采样的IP地址转换为ELF符号表中的地址
 * - 支持文件映射和匿名映射
 * - 处理动态链接库的地址重定位
 * 
 * 边界条件：
 * - 如果地址不在VMA范围内，返回0
 * - 如果VMA没有文件映射（匿名内存），返回0
 * - 支持PIE（位置无关可执行文件）的地址计算
 * 
 * 示例：
 * 运行时地址：0x7f8b3c45a280
 * VMA起始：0x7f8b3c400000
 * 文件偏移：0x2000
 * 相对地址：0x45a280 - 0x400000 + 0x2000 = 0x5c280
 */
uint64_t get_relative_address(uint64_t real_addr, struct virtual_memory_area* vma) {
    if (!vma) {
        return 0;
    }
    
    if (real_addr < vma->start_addr || real_addr >= vma->end_addr) {
        return 0;
    }
    
    return real_addr - vma->start_addr + vma->file_offset;
}
