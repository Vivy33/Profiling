/**
 * @file process.c
 * @brief 进程管理和虚拟内存区域(VMA)操作核心模块
 * 
 * 负责进程生命周期的完整管理，包括：
 * - 进程哈希表的创建、查找和维护
 * - 进程内存映射(/proc/[pid]/maps)的解析和缓存
 * - 虚拟内存区域(VMA)的红黑树管理
 * - 死进程的自动检测和清理
 * - ELF文件引用的自动释放
 * 
 * 关键数据结构：
 * - process_info: 进程的完整运行时画像
 * - virtual_memory_area: 进程地址空间的连续区间
 * - process_hash_table: PID到进程信息的O(1)查找映射
 * 
 * 内存管理：
 * - 使用哈希表+红黑树实现高效查找
 * - 完善的引用计数和自动清理机制
 * - 支持进程退出的优雅清理
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#include "../../include/header.h"
#include "../../include/hash.h"
#include "../../include/rbtree.h"
#include "../../include/config.h"

/**
 * @brief 从/proc/[pid]/stat获取进程启动时间
 * @param pid 进程ID
 * @return unsigned long long 进程启动时间（jiffies），失败返回0
 *
 * /proc/[pid]/stat文件包含多个字段，第22个字段是启动时间。
 * 这是一个可靠的标识符，用于区分PID复用的不同进程实例。
 */
unsigned long long get_process_start_time(int pid) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    unsigned long long start_time = 0;
    // /proc/[pid]/stat中的第22个字段是starttime（自Linux 2.6起）
    // 我们扫描前21个字段以获取它。
    int result = fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu", &start_time);
    fclose(f);

    if (result == 1) {
        return start_time;
    }
    return 0;
}

/**
 * @brief 读取/proc/[pid]/下的指定文件内容
 * @param pid 进程ID
 * @param file_name 要读取的文件名 (例如 "comm", "cmdline")
 * @return char* 文件内容的动态分配字符串，失败返回NULL
 * 
 * 读取/proc文件系统的核心辅助函数。
 * - 动态构建文件路径
 * - 一次性读取文件所有内容
 * - 调用者负责释放返回的字符串内存
 */
static char* read_proc_file(int pid, const char* file_name) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/%s", pid, file_name);

    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        return NULL; // 进程可能已退出
    }

    char* buffer = (char*)malloc(4096); // 通常足够大
    if (!buffer) {
        close(fd);
        return NULL;
    }

    ssize_t bytes_read = read(fd, buffer, 4095);
    close(fd);

    if (bytes_read <= 0) {
        free(buffer);
        return NULL;
    }

    buffer[bytes_read] = '\0';

    // 处理comm文件末尾的换行符
    if (strcmp(file_name, "comm") == 0 && bytes_read > 0 && buffer[bytes_read - 1] == '\n') {
        buffer[bytes_read - 1] = '\0';
    }

    // 处理cmdline文件中的\0分隔符
    if (strcmp(file_name, "cmdline") == 0) {
        for (ssize_t i = 0; i < bytes_read - 1; ++i) {
            if (buffer[i] == '\0') {
                buffer[i] = ' '; // 替换为为空格
            }
        }
    }

    return buffer;
}

/**
 * @brief 在进程哈希表中查找指定PID的进程
 * @param table 进程哈希表指针
 * @param pid 要查找的进程ID
 * @return struct process_info* 找到的进程信息，未找到返回NULL
 * 
 * 查找算法：
 * 1. 使用PID哈希值计算桶索引
 * 2. 在冲突链表中顺序查找
 * 3. 时间复杂度：O(1)平均，O(n)最坏情况
 * 
 * 哈希函数：hash_pid() - 将PID均匀分布在0-1023范围内
 */
struct process_info* find_process(struct process_hash_table* table, int pid) {
    unsigned int index = hash_pid(pid);
    struct process_hash_node* node = table->nodes[index];
    
    while (node) {
        if (node->process_data.process_id == pid) {
            return &node->process_data;
        }
        node = node->next_node;
    }
    return NULL;
}

/**
 * @brief 查找或创建进程信息（惰性加载模式）
 * @param process_table 进程哈希表指针
 * @param pid 进程ID
 * @return struct process_info* 进程信息，失败返回NULL
 * 
 * 工作流程：
 * 1. 先在哈希表中查找，找到直接返回
 * 2. 未找到时创建新的进程节点
 * 3. 解析/proc/[pid]/maps获取内存映射信息
 * 4. 将新进程插入哈希表
 * 
 * 内存分配：
 * - process_hash_node: 进程节点
 * - VMA树节点: 动态创建，包含在parse_process_memory_maps()中
 * 
 * 错误处理：
 * - 内存分配失败：返回NULL
 * - 内存映射解析失败：释放已分配内存
 */
struct process_info* find_new_process(struct process_hash_table* process_table, int pid) {
    // 优化说明：首先尝试从哈希表获取已有进程，避免重复解析；
    // 对短生命周期且已退出的进程，在尝试读取基本信息后快速返回，
    // 跳过 /proc/[pid]/maps 解析，减少 I/O 和解析开销。
    struct process_info* proc = find_process(process_table, pid);
    if (proc) {
        return proc;  // 进程已存在，直接返回缓存结果
    }

    // 创建新的进程节点
    struct process_hash_node* new_node = (struct process_hash_node*)calloc(1, sizeof(struct process_hash_node));
    if (!new_node) {
        perror("Failed to allocate memory for new process node");
        return NULL;
    }
    
    // 初始化进程基本信息
    new_node->process_data.process_id = pid;
    new_node->process_data.start_time = get_process_start_time(pid);
    new_node->process_data.process_name = read_proc_file(pid, "comm");
    new_node->process_data.command_line = read_proc_file(pid, "cmdline");

    // 回退逻辑，确保进程名和命令行不为NULL
    if (!new_node->process_data.process_name) {
        // 尝试从cmdline获取进程名
        if (new_node->process_data.command_line && new_node->process_data.command_line[0] != '\0') {
            char* space = strchr(new_node->process_data.command_line, ' ');
            if (space) *space = '\0';
            char* basename = strrchr(new_node->process_data.command_line, '/');
            new_node->process_data.process_name = strdup(basename ? basename + 1 : new_node->process_data.command_line);
            if (space) *space = ' ';
        } else {
            new_node->process_data.process_name = strdup("unknown");
        }
    } else {
        // 清理进程名称中的换行符
        char* newline = strchr(new_node->process_data.process_name, '\n');
        if (newline) *newline = '\0';

        // 如果进程名称为空或无效，使用PID作为标识
        if (strlen(new_node->process_data.process_name) == 0 ||
            strcmp(new_node->process_data.process_name, "\n") == 0) {
            char pid_str[32];
            snprintf(pid_str, sizeof(pid_str), "pid_%d", pid);
            free(new_node->process_data.process_name);
            new_node->process_data.process_name = strdup(pid_str);
        }
    }

    if (!new_node->process_data.command_line || new_node->process_data.command_line[0] == '\0') {
        free(new_node->process_data.command_line);
        new_node->process_data.command_line = strdup(new_node->process_data.process_name);
    }
    
    // 短生命周期进程优化：若进程已退出则跳过maps解析，避免不必要的开销
    if (!is_process_alive(pid)) {
        free(new_node->process_data.process_name);
        free(new_node->process_data.command_line);
        free(new_node);
        return NULL;
    }

    new_node->process_data.memory_map_tree = RB_ROOT;  // 初始化红黑树根节点

    // 解析进程内存映射，填充VMA树
    if (parse_process_memory_maps(&new_node->process_data) != 0) {
        free(new_node);
        return NULL;
    }

    // 插入进程到哈希表，头插法处理冲突
    unsigned int index = hash_pid(pid);
    new_node->next_node = process_table->nodes[index];
    process_table->nodes[index] = new_node;

    return &new_node->process_data;
}

/**
 * @brief 从哈希表中移除一个进程并释放其资源
 * @param sys 系统上下文
 * @param pid 要移除的进程ID
 *
 * 用于处理PID复用时，主动废弃过时的缓存条目。
 */
void remove_process(struct system_context *sys, int pid) {
    // 资源释放说明：ELF 文件的引用释放统一在 free_vma_tree 中进行，
    // 避免在此处重复释放导致的 double-free。此函数仅负责解除哈希链表链接
    // 并释放与进程元数据相关的字符串和节点内存。
    unsigned int index = hash_pid(pid);
    struct process_hash_node* node = sys->process_table->nodes[index];
    struct process_hash_node* prev = NULL;

    while (node) {
        if (node->process_data.process_id == pid) {
            // 从链表中解除节点链接
            if (prev) {
                prev->next_node = node->next_node;
            } else {
                sys->process_table->nodes[index] = node->next_node;
            }

            // 释放资源（ELF 引用统一由 free_vma_tree 处理，避免重复释放）
            // 修复 use-after-free：free_vma_tree 在释放 VMA 期间可能会读取/使用进程的元信息，
            // 因此必须先释放 VMA，再释放字符串等元数据，避免释放后仍被访问。
            free_vma_tree(&node->process_data.memory_map_tree);

            free(node->process_data.process_name);
            node->process_data.process_name = NULL;
            free(node->process_data.command_line);
            node->process_data.command_line = NULL;
            free(node->process_data.executable_path);
            node->process_data.executable_path = NULL;
            free(node);
            return;
        }
        prev = node;
        node = node->next_node;
    }
}

/**
 * @brief 在进程的VMA树中查找指定地址所属的内存区域
 * @param proc 进程信息指针
 * @param real_addr 要查找的内存地址
 * @return struct virtual_memory_area* VMA信息，未找到返回NULL
 * 
 * 查找算法：
 * 1. 使用红黑树实现O(log n)查找
 * 2. 比较地址与VMA的起始和结束地址
 * 3. 支持匿名内存映射（如heap、stack）的查找
 * 
 * 地址范围验证：
 * - 检查地址是否在VMA的[start_addr, end_addr)范围内
 * - 支持文件映射和匿名映射
 * 
 * 典型场景：
 * - 代码段：/usr/bin/nginx
 * - 共享库：/lib/x86_64-linux-gnu/libc.so.6
 * - 堆：[heap]
 * - 栈：[stack]
 */
struct virtual_memory_area* find_vma_from_process(struct process_info* proc, unsigned long real_addr) {
    if (!proc) return NULL;
    struct rb_node* node = proc->memory_map_tree.rb_node;

    while (node) {
        struct virtual_memory_area* vma_info = rb_entry(node, struct virtual_memory_area, vm_rb_node);

        if (real_addr < vma_info->start_addr)
            node = node->rb_left;  // 地址在左子树
        else if (real_addr >= vma_info->end_addr)
            node = node->rb_right;  // 地址在右子树
        else
            return vma_info;  // 找到包含地址的VMA
    }
    return NULL;  // 地址不在任何已知VMA范围内
}

/**
 * @brief 检查指定PID的进程是否仍然存活
 * @param pid 进程ID
 * @return bool true=进程存活，false=进程已退出
 * 
 * 实现原理：
 * - 使用kill(pid, 0)系统调用检查进程存在性
 * - 不会向进程发送实际信号，仅检查权限和进程存在
 * - 适用于非root用户（检查自身进程）
 */
bool is_process_alive(int pid) {
    return (kill(pid, 0) == 0);
}

/**
 * @brief 定期清理已退出的进程及其资源
 * @param sys 系统上下文指针
 * 
 * 清理流程：
 * 1. 遍历所有哈希桶
 * 2. 检查每个进程的存活状态
 * 3. 清理已退出进程的所有资源：
 *    - 释放ELF文件引用（调用release_elf）
 *    - 释放进程名称、命令行、路径内存
 *    - 释放VMA红黑树
 *    - 释放进程节点本身
 * 4. 处理哈希链表中的节点移除
 * 
 * 线程安全：应在主线程中调用，避免并发访问
 * 触发频率：由global_config.cleanup_interval配置（默认5秒）
 */
void cleanup_dead_processes(struct system_context *sys) {
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        struct process_hash_node* node = sys->process_table->nodes[i];
        struct process_hash_node* prev = NULL;
        
        while (node) {
            // pid复用 概率极低
            if (!is_process_alive(node->process_data.process_id)) {
                // 从哈希链表中移除节点
                struct process_hash_node* temp = node;
                if (prev) {
                    prev->next_node = node->next_node;
                    node = prev->next_node;
                } else {
                    sys->process_table->nodes[i] = node->next_node;
                    node = sys->process_table->nodes[i];
                }

                // 释放进程相关内存（ELF 引用由 free_vma_tree 统一处理）
                // 修复 use-after-free：先释放 VMA 树，再释放进程元数据字符串。
                free_vma_tree(&temp->process_data.memory_map_tree);  // 释放VMA红黑树
                
                free(temp->process_data.process_name);
                temp->process_data.process_name = NULL;
                free(temp->process_data.command_line);
                temp->process_data.command_line = NULL;
                free(temp->process_data.executable_path);
                temp->process_data.executable_path = NULL;
                free(temp);
            } else {
                prev = node;
                node = node->next_node;
            }
        }
    }
}

/**
 * @brief 释放整个进程哈希表及其所有内容
 * @param hash_table 要释放的哈希表指针
 * 
 * 清理操作：
 * 1. 遍历所有哈希桶（0-1023）
 * 2. 释放每个桶中的链表节点
 * 3. 释放每个进程的所有关联内存
 * 4. 释放哈希表结构本身
 * 
 * 安全性：
 * - 支持NULL指针安全调用
 * - 确保所有内存都被正确释放
 * - 处理可能的循环引用（如ELF文件引用已在cleanup_dead_processes中处理）
 */
void free_process_hash_table(struct process_hash_table *hash_table) {
    if (!hash_table) return;
    
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        struct process_hash_node* node = hash_table->nodes[i];
        while (node) {
            struct process_hash_node* temp = node;
            node = node->next_node;
            
            // 释放VMA红黑树（可能持有 ELF 引用，需优先释放）
            free_vma_tree(&temp->process_data.memory_map_tree);
            
            // 释放进程相关的所有字符串内存
            free(temp->process_data.process_name);
            temp->process_data.process_name = NULL;
            free(temp->process_data.command_line);
            temp->process_data.command_line = NULL;
            free(temp->process_data.executable_path);
            temp->process_data.executable_path = NULL;
            
            free(temp);
        }
    }
    free(hash_table);
}