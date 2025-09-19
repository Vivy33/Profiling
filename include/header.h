#ifndef HEADER_H
#define HEADER_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <libelf.h>
#include <gelf.h>
#include <elf.h>

#define HASHTABLE_SIZE 1024

#include "rbtree.h"
#include "hash.h"
#include "perf.h"
#include "kernel_symbol.h"

#define READ 0x1
#define WRITE 0x2
#define EXECUTE 0x4

// Virtual Memory Area - 表示进程的虚拟内存区域
struct virtual_memory_area {
    uint64_t start_addr;      // 区域起始地址
    uint64_t end_addr;        // 区域结束地址
    unsigned int vm_flags;    // 内存区域权限标志（读/写/执行）
    uint64_t file_offset;     // 映射文件的偏移量
    char *mapping_name;       // 内存映射的名称（如文件路径或匿名映射）
    struct elf_file* elf_file; // 指向关联ELF文件缓存的指针
    struct rb_node vm_rb_node; // 红黑树节点，用于快速查找
};

// 进程信息结构体 - 表示一个正在运行的进程
struct process_info {
    int process_id;           // 进程ID
    unsigned long long start_time; // 进程启动时间（jiffies）
    char* process_name;       // 进程名称
    char* command_line;       // 完整命令行参数
    char* executable_path;    // 可执行文件完整路径
    struct rb_root memory_map_tree; // 虚拟内存区域的红黑树
};

// 进程哈希表节点 - 用于快速查找进程
struct process_hash_node {
    struct process_info process_data;    // 进程数据
    struct process_hash_node* next_node; // 链表法解决哈希冲突
};

// Process hash table
struct process_hash_table {
    struct process_hash_node* nodes[HASHTABLE_SIZE];
};

// 符号信息结构体 - 表示ELF文件中的符号（函数或变量）
struct symbol_info {
    char* symbol_name;        // 符号名称
    uint64_t symbol_start;    // 符号起始地址
    uint64_t symbol_size;     // 符号大小（字节为单位）
    struct rb_node symbol_rb_node; // 红黑树节点，用于符号快速查找
};

// ELF符号集合 - 存储一个ELF文件中的所有符号
struct elf_symbol_collection {
    int total_symbols;        // 符号总数
    struct rb_root symbol_tree; // 符号红黑树的根节点
};

// ELF文件结构体 - 表示一个ELF可执行文件或库文件
struct elf_file {
    char* file_path;          // 文件完整路径
    char* build_id;           // ELF文件的唯一构建ID
    int reference_count;      // 引用计数（用于缓存管理）
    struct elf_symbol_collection* symbols; // ELF文件中的符号集合
    Elf64_Ehdr elf_header;    // ELF文件头
    Elf64_Phdr *program_headers; // 程序头表
    Elf64_Shdr *section_headers; // 节区头表
    char *section_string_table; // 节区字符串表
};

// ELF文件哈希表节点 - 用于ELF文件缓存
struct elf_file_hash_node {
    struct elf_file elf_file_data;  // ELF文件数据
    struct elf_file_hash_node* next_node; // 链表法解决哈希冲突
};

// ELF文件哈希表 - 用于快速查找ELF文件
struct elf_file_cache {
    struct elf_file_hash_node* cache_buckets[HASHTABLE_SIZE]; // 哈希桶
};

// 系统全局信息 - 管理整个系统的进程和ELF文件
struct system_context {
    struct process_hash_table* process_table;  // 进程哈希表
    struct elf_file_cache* elf_cache;          // ELF文件缓存
    struct rb_root *kernel_symbols;            // 内核符号红黑树
};

// 调用栈解析结果
struct callchain_result {
    uint32_t pid, tid;
    uint64_t ip;
    uint64_t nr;
    uint64_t *ips;
};

struct sample_data {
    struct perf_event_header header;
    uint32_t pid, tid;
    uint64_t ip;
};

// Function prototypes

// main_loop.c
void main_loop(struct system_context* system_info, struct perf_event_manager* manager);

// system.c
int initialize_system(struct system_context* system_info);
void cleanup_system(struct system_context* system_info);

// process.c
struct process_info* find_process(struct process_hash_table* table, int pid);
struct process_info* find_new_process(struct process_hash_table* process_table, int pid);
struct virtual_memory_area* find_vma_from_process(struct process_info* proc, unsigned long real_addr);
void cleanup_dead_processes(struct system_context *sys);
void free_process_hash_table(struct process_hash_table *hash_table);
void free_vma_tree(struct rb_root *root);
bool is_process_alive(int pid);
unsigned long long get_process_start_time(int pid);
void remove_process(struct system_context *sys, int pid);

// handler.c
void parse_sample_data(struct perf_event_header *header, struct callchain_result *result, uint64_t max_ips);
void symbolize_sample(struct system_context *sys, struct callchain_result *callchain);

// symbol_table.c
struct symbol_info* rb_search_symbol(struct rb_root *root, uint64_t addr);
const char* find_symbol_name_from_elf(struct elf_file* elf, uint64_t relative_address);

// elf.c
struct elf_file* find_or_create_elf(struct system_context* sys_ctx, int pid, const char* filename);
void release_elf_by_ptr(struct elf_file_cache* elf_cache, struct elf_file* elf_obj);
void clear_elf_cache(struct elf_file_cache* elf_cache);

// vma.c
int parse_process_memory_maps(struct process_info* proc);
uint64_t get_relative_address(uint64_t real_addr, struct virtual_memory_area* vma);

#endif // HEADER_H