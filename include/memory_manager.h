#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

#include "../include/header.h"
#include "elf_cache.h"
#include "vma_cache.h"
#include "symbol_cache.h"

// 内存管理配置
#define MM_CONFIG_DEFAULT_CACHE_SIZE 1024
#define MM_CONFIG_DEFAULT_VMA_CACHE_SIZE 64
#define MM_CONFIG_DEFAULT_SYMBOL_CACHE_SIZE 256

// 内存管理器统合结构
struct memory_manager {
    int initialized;                    // 初始化标志
    struct memory_cache* file_cache;    // 文件缓存
    struct vma_cache_manager* vma_cache; // VMA缓存
    struct symbol_cache_manager* symbol_cache; // 符号缓存

    // 统计信息
    struct {
        uint64_t total_accesses;        // 总访问次数
        uint64_t cache_hits;            // 缓存命中次数
        uint64_t cache_misses;          // 缓存未命中次数
        uint64_t cache_evictions;       // 缓存淘汰次数
    } stats;

};

// 全局内存管理器
extern struct memory_manager global_memory_manager;

// 初始化内存管理器
int memory_manager_init(void);

// 清理内存管理器
void memory_manager_cleanup(void);

// 内存映射文件（带缓存优化）
void* memory_manager_mmap_file(const char* filename, size_t* file_size);

// 取消内存映射
int memory_manager_munmap_file(void* addr, size_t length);

// 从文件读取数据（带缓存）
int memory_manager_read_file(const char* filename, uint64_t offset, void* buffer, size_t size);

// 查找VMA信息（带缓存）
struct vma_cache_entry* memory_manager_find_vma(uint64_t addr);

// 查找符号信息（带缓存）
const char* memory_manager_find_symbol(uint64_t addr, const char* filename);

// 获取相对地址（带缓存优化）
uint64_t memory_manager_get_relative_addr(uint64_t real_addr, const char* filename);


// 获取内存管理统计信息
void memory_manager_get_stats(uint64_t* hits, uint64_t* misses, uint64_t* evictions);

// 强制刷新所有缓存
void memory_manager_flush_all(void);

// 内存使用信息
void memory_manager_print_usage(void);

#endif