#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include "../include/header.h"


struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline int list_empty(const struct list_head *head) {
    return head->next == head;
}

static inline void list_add_tail(struct list_head *new_node, struct list_head *head) {
    new_node->next = head;
    new_node->prev = head->prev;
    head->prev->next = new_node;
    head->prev = new_node;
}

static inline void list_del(struct list_head *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = NULL;
    entry->prev = NULL;
}

/**
 * @brief 通过结构体成员指针获取其所在结构体的指针
 * @param ptr 指向结构体中某个成员的指针
 * @param type 包含该成员的结构体类型
 * @param member 结构体中该成员的名称
 * @return 指向包含该成员的整个结构体的指针
 */
#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/**
 * @brief 获取链表中第一个数据项的指针
 * @param ptr 链表头节点（struct list_head）的指针
 * @param type 链表数据项的结构体类型
 * @param member 链表数据项结构体中struct list_head成员的名称
 * @return 指向链表中第一个数据项的指针
 */
#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)

/**
 * @brief 安全地遍历链表，允许在遍历过程中删除当前节点
 * @param pos 当前遍历到的链表节点（struct list_head）的指针
 * @param n pos的下一个链表节点（struct list_head）的指针，作为“预读”指针
 * @param head 链表头节点（struct list_head）的指针
 */
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)

#define CACHE_BLOCK_SIZE 4096  // 4KB缓存块大小
#define MAX_CACHE_BLOCKS 1024  // 最大缓存块数量

// 缓存块结构
struct cache_block {
    uint64_t offset;           // 文件偏移
    char data[CACHE_BLOCK_SIZE]; // 数据缓存
    struct rb_node cache_rb_node; // RB树节点
    struct list_head lru_list;    // LRU链表
    int ref_count;            // 引用计数
    uint64_t last_access;     // 最后访问时间
};

// 内存缓存管理器
struct memory_cache {
    struct rb_root cache_tree;    // RB树索引
    struct list_head lru_list;    // LRU链表头
    int current_blocks;           // 当前缓存块数量
    int max_blocks;               // 最大缓存块数量
    int fd;                       // 文件描述符
    size_t file_size;             // 文件大小
    uint64_t access_counter;      // 访问计数器
};

// 初始化内存缓存
struct memory_cache* cache_init(int fd, size_t file_size);

// 清理内存缓存
void cache_cleanup(struct memory_cache* cache);

// 从缓存读取数据
int cache_read(struct memory_cache* cache, uint64_t offset, void* buffer, size_t size);

// 写入数据到缓存
int cache_write(struct memory_cache* cache, uint64_t offset, const void* buffer, size_t size);

// 预取数据到缓存
int cache_prefetch(struct memory_cache* cache, uint64_t offset, size_t size);

// 获取缓存统计信息
void cache_get_stats(struct memory_cache* cache, int* hits, int* misses, int* evictions);

// 强制刷新缓存
void cache_flush(struct memory_cache* cache);

// 基于collector实现的ELF缓存结构
struct elf_cache_entry {
    char* filename;           // 文件名
    int fd;                   // 文件描述符
    void* mmap_base;          // 内存映射基址
    size_t mmap_size;         // 映射大小
    struct memory_cache* cache; // 块缓存
    struct rb_root symbol_cache; // 符号缓存
    struct list_head lru_list;   // LRU链表
    int ref_count;            // 引用计数
    uint64_t last_access;     // 最后访问时间
};

// ELF缓存管理器
struct elf_cache_manager {
    struct elf_cache_entry* entries[MAX_CACHE_BLOCKS];
    int count;
    int max_count;
    struct list_head lru_list;
};

// 全局ELF缓存管理器
extern struct elf_cache_manager global_elf_cache;

// 初始化ELF缓存管理器
int elf_cache_init(int max_entries);

// 获取ELF缓存条目
struct elf_cache_entry* elf_cache_get(const char* filename);

// 释放ELF缓存条目
void elf_cache_put(struct elf_cache_entry* entry);

// 清理ELF缓存
void elf_cache_cleanup(void);

#endif
