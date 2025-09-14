#ifndef SYMBOL_CACHE_H
#define SYMBOL_CACHE_H

#include "../include/header.h"
#include "elf_cache.h"
#include <stdlib.h>

#define SYMBOL_CACHE_SIZE 256  // 符号缓存大小
#define SYMBOL_CACHE_BLOCK_SIZE 64  // 符号缓存块大小

// 符号缓存条目
struct symbol_cache_entry {
    uint64_t addr;            // 符号地址
    char* name;               // 符号名称
    uint64_t size;            // 符号大小
    char* filename;           // 所属文件名
    struct rb_node symbol_rb_node; // RB树节点
    struct list_head lru_list;     // LRU链表
    uint64_t last_access;     // 最后访问时间
    int ref_count;            // 引用计数
};

// 符号缓存块（批量加载）
struct symbol_cache_block {
    uint64_t start_addr;      // 起始地址
    uint64_t end_addr;        // 结束地址
    struct symbol_cache_entry entries[SYMBOL_CACHE_BLOCK_SIZE];
    int entry_count;          // 实际条目数
    struct rb_node block_rb_node;  // RB树节点
    struct list_head lru_list;     // LRU链表
    uint64_t last_access;     // 最后访问时间
};

// 符号缓存管理器
struct symbol_cache_manager {
    struct rb_root symbol_tree;   // 符号地址树
    struct rb_root block_tree;    // 符号块树
    struct list_head lru_list;    // LRU链表
    int current_symbols;          // 当前符号数量
    int max_symbols;              // 最大符号数量
    uint64_t access_counter;      // 访问计数器
};

// 初始化符号缓存
int symbol_cache_init(int max_symbols);

// 清理符号缓存
void symbol_cache_cleanup(void);

// 查找符号缓存
struct symbol_cache_entry* symbol_cache_lookup(uint64_t addr);

// 添加符号到缓存
int symbol_cache_add(const char* name, uint64_t addr, uint64_t size, const char* filename);

// 批量添加符号块
int symbol_cache_add_block(struct symbol_info* symbols, int count, const char* filename);

// 移除符号缓存
void symbol_cache_remove(uint64_t addr);

// 释放符号缓存条目
void symbol_cache_put(struct symbol_cache_entry* entry);

// 获取符号名称（带缓存优化）
const char* symbol_cache_get_name(uint64_t addr, const char* filename);

// 强制刷新符号缓存
void symbol_cache_flush(void);

// 获取缓存统计信息
void symbol_cache_get_stats(int* hits, int* misses, int* evictions);

// 延迟加载符号表
int symbol_cache_lazy_load(const char* filename, uint64_t addr);

#endif