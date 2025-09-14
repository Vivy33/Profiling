#ifndef VMA_CACHE_H
#define VMA_CACHE_H

#include "../include/header.h"
#include "elf_cache.h"
#include <stdlib.h>

#define VMA_CACHE_SIZE 64  // VMA缓存大小

// VMA缓存条目
struct vma_cache_entry {
    uint64_t start_addr;      // 起始地址
    uint64_t end_addr;        // 结束地址
    uint64_t file_offset;     // 文件偏移
    char* mapping_name;       // 映射文件名
    int vm_flags;             // 权限标志
    struct rb_node vma_rb_node; // RB树节点
    struct list_head lru_list;  // LRU链表
    uint64_t last_access;     // 最后访问时间
    int ref_count;            // 引用计数
};

// VMA缓存管理器
struct vma_cache_manager {
    struct rb_root vma_tree;      // VMA地址树
    struct list_head lru_list;    // LRU链表
    int current_count;            // 当前缓存数量
    int max_count;                // 最大缓存数量
    uint64_t access_counter;      // 访问计数器
};

// 初始化VMA缓存
int vma_cache_init(int max_entries);

// 清理VMA缓存
void vma_cache_cleanup(void);

// 查找VMA缓存
struct vma_cache_entry* vma_cache_lookup(uint64_t addr);

// 添加VMA到缓存
int vma_cache_add(struct virtual_memory_area* vma);

// 移除VMA缓存
void vma_cache_remove(uint64_t start_addr);

// 更新VMA缓存
int vma_cache_update(uint64_t start_addr, struct virtual_memory_area* vma);

// 获取地址对应的VMA
struct vma_cache_entry* vma_cache_find_vma(uint64_t addr);

// 获取相对地址（带缓存优化）
uint64_t vma_cache_get_relative_addr(uint64_t real_addr, const char* filename);

// 强制刷新VMA缓存
void vma_cache_flush(void);

// 获取缓存统计信息
void vma_cache_get_stats(int* hits, int* misses, int* evictions);

#endif