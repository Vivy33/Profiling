#include "elf_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

// 全局ELF缓存管理器
struct elf_cache_manager global_elf_cache = {0};

// 初始化内存缓存
struct memory_cache* cache_init(int fd, size_t file_size) {
    struct memory_cache* cache = (struct memory_cache*)calloc(1, sizeof(struct memory_cache));
    if (!cache) {
        return NULL;
    }

    cache->cache_tree = RB_ROOT;
    INIT_LIST_HEAD(&cache->lru_list);
    cache->current_blocks = 0;
    cache->max_blocks = MAX_CACHE_BLOCKS;
    cache->fd = fd;
    cache->file_size = file_size;
    cache->access_counter = 0;

    return cache;
}

// 清理内存缓存
void cache_cleanup(struct memory_cache* cache) {
    if (!cache) return;

    // 清理RB树中所有缓存块
    struct rb_node* node = rb_first(&cache->cache_tree);
    while (node) {
        struct cache_block* block = rb_entry(node, struct cache_block, cache_rb_node);
        struct rb_node* next = rb_next(node);
        rb_erase(node, &cache->cache_tree);
        free(block);
        node = next;
    }

    // 清理LRU链表
    struct list_head* pos, *tmp;
    list_for_each_safe(pos, tmp, &cache->lru_list) {
        struct cache_block* block = list_entry(pos, struct cache_block, lru_list);
        list_del(pos);
        free(block);
    }

    free(cache);
}

// 查找缓存块（RB树查找）
static struct cache_block* find_cache_block(struct memory_cache* cache, uint64_t offset) {
    struct rb_node* node = cache->cache_tree.rb_node;

    while (node) {
        struct cache_block* block = rb_entry(node, struct cache_block, cache_rb_node);

        if (offset < block->offset) {
            node = node->rb_left;
        } else if (offset >= block->offset + CACHE_BLOCK_SIZE) {
            node = node->rb_right;
        } else {
            return block;
        }
    }

    return NULL;
}

// 从文件读取数据块
static int read_from_file(struct memory_cache* cache, uint64_t offset, void* buffer, size_t size) {
    if (lseek(cache->fd, offset, SEEK_SET) == (off_t)-1) {
        return -1;
    }

    ssize_t bytes_read = read(cache->fd, buffer, size);
    if (bytes_read != (ssize_t)size) {
        return -1;
    }

    return 0;
}

// 创建新的缓存块
static struct cache_block* create_cache_block(struct memory_cache* cache, uint64_t offset) {
    struct cache_block* block = (struct cache_block*)calloc(1, sizeof(struct cache_block));
    if (!block) {
        return NULL;
    }

    block->offset = offset;
    block->ref_count = 0;
    block->last_access = ++cache->access_counter;
    INIT_LIST_HEAD(&block->lru_list);

    // 从文件读取数据
    size_t read_size = CACHE_BLOCK_SIZE;
    if (offset + CACHE_BLOCK_SIZE > cache->file_size) {
        read_size = cache->file_size - offset;
    }

    if (read_from_file(cache, offset, block->data, read_size) != 0) {
        free(block);
        return NULL;
    }

    return block;
}

// 插入缓存块到RB树
static void insert_cache_block(struct memory_cache* cache, struct cache_block* block) {
    struct rb_node **new_node = &cache->cache_tree.rb_node, *parent = NULL;

    while (*new_node) {
        struct cache_block* current_block = rb_entry(*new_node, struct cache_block, cache_rb_node);
        parent = *new_node;

        if (block->offset < current_block->offset) {
            new_node = &((*new_node)->rb_left);
        } else {
            new_node = &((*new_node)->rb_right);
        }
    }

    rb_link_node(&block->cache_rb_node, parent, new_node);
    rb_insert_color(&block->cache_rb_node, &cache->cache_tree);
}

// 淘汰最老的缓存块（LRU策略）
static void evict_lru_block(struct memory_cache* cache) {
    if (list_empty(&cache->lru_list)) {
        return;
    }

    struct cache_block* lru_block = list_first_entry(&cache->lru_list, struct cache_block, lru_list);

    rb_erase(&lru_block->cache_rb_node, &cache->cache_tree);
    list_del(&lru_block->lru_list);
    free(lru_block);
    cache->current_blocks--;
}

// 从缓存读取数据
int cache_read(struct memory_cache* cache, uint64_t offset, void* buffer, size_t size) {
    if (!cache || !buffer || offset >= cache->file_size) {
        return -1;
    }

    // 确保读取的长度不超过文件实际大小
    if (offset + size > cache->file_size) {
        size = cache->file_size - offset;
    }

    // uint64_t start_block = offset / CACHE_BLOCK_SIZE; // 计算数据起始位置所在的缓存块索引
    // uint64_t end_block = (offset + size - 1) / CACHE_BLOCK_SIZE; // 计算数据结束位置所在的缓存块索引

    char* buf = (char*)buffer;
    uint64_t remaining = size;
    uint64_t current_offset = offset;

    while (remaining > 0) {
        uint64_t block_offset = (current_offset / CACHE_BLOCK_SIZE) * CACHE_BLOCK_SIZE;
        struct cache_block* block = find_cache_block(cache, current_offset);

        if (!block) {
            // 缓存未命中，需要加载
            if (cache->current_blocks >= cache->max_blocks) {
                evict_lru_block(cache);
            }

            block = create_cache_block(cache, block_offset);
            if (!block) {
                return -1;
            }

            insert_cache_block(cache, block);
            list_add_tail(&block->lru_list, &cache->lru_list);
            cache->current_blocks++;
        }

        // 更新访问时间并移动到LRU链表尾部
        block->last_access = ++cache->access_counter;
        list_del(&block->lru_list);
        list_add_tail(&block->lru_list, &cache->lru_list);

        // 计算块内偏移和复制大小
        uint64_t block_pos = current_offset % CACHE_BLOCK_SIZE;
        // 计算在当前缓存块中可以复制的数据量
        // 取剩余未复制的总数据量和当前缓存块中剩余空间两者的最小值
        uint64_t copy_size;
        if (remaining < (CACHE_BLOCK_SIZE - block_pos)) {
            copy_size = remaining;
        } else {
            copy_size = (CACHE_BLOCK_SIZE - block_pos);
        }

        memcpy(buf, block->data + block_pos, copy_size);

        buf += copy_size;
        current_offset += copy_size;
        remaining -= copy_size;
    }

    return 0;
}

// 写入数据到缓存
int cache_write(struct memory_cache* cache, uint64_t offset, const void* buffer, size_t size) {
    // 只读缓存，不支持写入
    return -1;
}

// 预取数据到缓存
int cache_prefetch(struct memory_cache* cache, uint64_t offset, size_t size) {
    if (!cache) return -1;

    // 预取策略：读取数据但不返回给用户
    char dummy[CACHE_BLOCK_SIZE];
    uint64_t current_offset = offset;

    while (current_offset < offset + size) {
        if (cache_read(cache, current_offset, dummy, sizeof(dummy)) != 0) {
            return -1;
        }
        current_offset += CACHE_BLOCK_SIZE;
    }

    return 0;
}

// 获取缓存统计信息
void cache_get_stats(struct memory_cache* cache, int* hits, int* misses, int* evictions) {
    if (!cache) return;

    // 简化统计：实际应该跟踪命中/未命中
    *hits = cache->current_blocks;  // 实际应该通过访问跟踪实现
    *misses = 0;
    *evictions = 0;
}

// 强制刷新缓存
void cache_flush(struct memory_cache* cache) {
    if (!cache) return;

    struct rb_node* node = rb_first(&cache->cache_tree);
    while (node) {
        struct cache_block* block = rb_entry(node, struct cache_block, cache_rb_node);
        struct rb_node* next = rb_next(node);
        rb_erase(node, &cache->cache_tree);
        list_del(&block->lru_list);
        free(block);
        node = next;
    }

    cache->current_blocks = 0;
}

// 初始化ELF缓存管理器
int elf_cache_init(int max_entries) {
    global_elf_cache.max_count = max_entries;
    global_elf_cache.count = 0;
    INIT_LIST_HEAD(&global_elf_cache.lru_list);

    for (int i = 0; i < MAX_CACHE_BLOCKS; i++) {
        global_elf_cache.entries[i] = NULL;
    }

    return 0;
}

// 获取ELF缓存条目
struct elf_cache_entry* elf_cache_get(const char* filename) {
    if (!filename) return NULL;

    

    // 查找已存在的条目
    for (int i = 0; i < global_elf_cache.count; i++) {
        if (global_elf_cache.entries[i] &&
            strcmp(global_elf_cache.entries[i]->filename, filename) == 0) {
            global_elf_cache.entries[i]->ref_count++;
            global_elf_cache.entries[i]->last_access = time(NULL);

            // 移动到LRU链表尾部
            list_del(&global_elf_cache.entries[i]->lru_list);
            list_add_tail(&global_elf_cache.entries[i]->lru_list, &global_elf_cache.lru_list);

            
            return global_elf_cache.entries[i];
        }
    }

    
    return NULL;
}

// 释放ELF缓存条目
void elf_cache_put(struct elf_cache_entry* entry) {
    if (!entry) return;

    
    entry->ref_count--;

    if (entry->ref_count == 0) {
        // 可以清理该条目
        // 实际实现中应该延迟清理或使用定时器
    }

    
}

// 清理ELF缓存
void elf_cache_cleanup(void) {
    

    for (int i = 0; i < global_elf_cache.count; i++) {
        if (global_elf_cache.entries[i]) {
            if (global_elf_cache.entries[i]->mmap_base) {
                munmap(global_elf_cache.entries[i]->mmap_base,
                       global_elf_cache.entries[i]->mmap_size);
            }
            if (global_elf_cache.entries[i]->fd >= 0) {
                close(global_elf_cache.entries[i]->fd);
            }
            cache_cleanup(global_elf_cache.entries[i]->cache);
            free(global_elf_cache.entries[i]->filename);
            free(global_elf_cache.entries[i]);
        }
    }

    global_elf_cache.count = 0;
    
    
}
