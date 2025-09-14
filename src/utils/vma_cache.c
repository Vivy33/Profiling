#include "vma_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

// 全局VMA缓存管理器
static struct vma_cache_manager global_vma_cache = {0};

// 初始化VMA缓存
int vma_cache_init(int max_entries) {
    global_vma_cache.vma_tree = RB_ROOT;
    INIT_LIST_HEAD(&global_vma_cache.lru_list);
    global_vma_cache.current_count = 0;
    global_vma_cache.max_count = max_entries;
    global_vma_cache.access_counter = 0;
    

    return 0;
}

// 清理VMA缓存
void vma_cache_cleanup(void) {
    

    struct rb_node* node = rb_first(&global_vma_cache.vma_tree);
    while (node) {
        struct vma_cache_entry* entry = rb_entry(node, struct vma_cache_entry, vma_rb_node);
        struct rb_node* next = rb_next(node);
        rb_erase(node, &global_vma_cache.vma_tree);
        free(entry->mapping_name);
        free(entry);
        node = next;
    }

    struct list_head* pos, *tmp;
    list_for_each_safe(pos, tmp, &global_vma_cache.lru_list) {
        struct vma_cache_entry* entry = list_entry(pos, struct vma_cache_entry, lru_list);
        list_del(pos);
        free(entry->mapping_name);
        free(entry);
    }

    global_vma_cache.current_count = 0;
    
    
}

// 查找VMA缓存条目（RB树查找）
static struct vma_cache_entry* find_vma_entry(uint64_t addr) {
    struct rb_node* node = global_vma_cache.vma_tree.rb_node;

    while (node) {
        struct vma_cache_entry* entry = rb_entry(node, struct vma_cache_entry, vma_rb_node);

        if (addr < entry->start_addr) {
            node = node->rb_left;
        } else if (addr >= entry->end_addr) {
            node = node->rb_right;
        } else {
            return entry;
        }
    }

    return NULL;
}

// 查找VMA缓存
struct vma_cache_entry* vma_cache_lookup(uint64_t addr) {
    

    struct vma_cache_entry* entry = find_vma_entry(addr);
    if (entry) {
        entry->last_access = ++global_vma_cache.access_counter;
        entry->ref_count++;

        // 移动到LRU链表尾部
        list_del(&entry->lru_list);
        list_add_tail(&entry->lru_list, &global_vma_cache.lru_list);
    }

    
    return entry;
}

// 淘汰最老的VMA缓存（LRU策略）
static void evict_lru_vma(void) {
    if (list_empty(&global_vma_cache.lru_list)) {
        return;
    }

    struct vma_cache_entry* lru_entry = list_first_entry(
        &global_vma_cache.lru_list, struct vma_cache_entry, lru_list);

    rb_erase(&lru_entry->vma_rb_node, &global_vma_cache.vma_tree);
    list_del(&lru_entry->lru_list);
    free(lru_entry->mapping_name);
    free(lru_entry);
    global_vma_cache.current_count--;
}

// 添加VMA到缓存
int vma_cache_add(struct virtual_memory_area* vma) {
    if (!vma) return -1;

    

    // 检查是否已存在
    struct vma_cache_entry* existing = find_vma_entry(vma->start_addr);
    if (existing) {
        
        return 0;  // 已存在
    }

    // 如果缓存满了，淘汰最老的
    if (global_vma_cache.current_count >= global_vma_cache.max_count) {
        evict_lru_vma();
    }

    struct vma_cache_entry* entry = calloc(1, sizeof(struct vma_cache_entry));
    if (!entry) {
        
        return -1;
    }

    entry->start_addr = vma->start_addr;
    entry->end_addr = vma->end_addr;
    entry->file_offset = vma->file_offset;
    entry->vm_flags = vma->vm_flags;
    entry->last_access = ++global_vma_cache.access_counter;
    entry->ref_count = 1;
    INIT_LIST_HEAD(&entry->lru_list);

    if (vma->mapping_name) {
        entry->mapping_name = strdup(vma->mapping_name);
        if (!entry->mapping_name) {
            free(entry);
            
            return -1;
        }
    } else {
        entry->mapping_name = NULL;
    }

    // 插入RB树
    struct rb_node **new = &global_vma_cache.vma_tree.rb_node, *parent = NULL;
    while (*new) {
        struct vma_cache_entry* this = rb_entry(*new, struct vma_cache_entry, vma_rb_node);
        parent = *new;

        if (entry->start_addr < this->start_addr) {
            new = &((*new)->rb_left);
        } else {
            new = &((*new)->rb_right);
        }
    }

    rb_link_node(&entry->vma_rb_node, parent, new);
    rb_insert_color(&entry->vma_rb_node, &global_vma_cache.vma_tree);

    // 添加到LRU链表
    list_add_tail(&entry->lru_list, &global_vma_cache.lru_list);
    global_vma_cache.current_count++;

    
    return 0;
}

// 移除VMA缓存
void vma_cache_remove(uint64_t start_addr) {
    

    struct vma_cache_entry* entry = find_vma_entry(start_addr);
    if (entry) {
        rb_erase(&entry->vma_rb_node, &global_vma_cache.vma_tree);
        list_del(&entry->lru_list);
        free(entry->mapping_name);
        free(entry);
        global_vma_cache.current_count--;
    }

    
}

// 更新VMA缓存
int vma_cache_update(uint64_t start_addr, struct virtual_memory_area* vma) {
    vma_cache_remove(start_addr);
    return vma_cache_add(vma);
}

// 获取地址对应的VMA（带缓存优化）
struct vma_cache_entry* vma_cache_find_vma(uint64_t addr) {
    return vma_cache_lookup(addr);
}

// 获取相对地址（带缓存优化）
uint64_t vma_cache_get_relative_addr(uint64_t real_addr, const char* filename) {
    struct vma_cache_entry* vma = vma_cache_lookup(real_addr);
    if (!vma) {
        return 0;
    }

    if (!vma->mapping_name || strcmp(vma->mapping_name, filename) != 0) {
        vma->ref_count--;
        return 0;
    }

    uint64_t relative_addr = real_addr - vma->start_addr + vma->file_offset;
    vma->ref_count--;

    return relative_addr;
}

// 强制刷新VMA缓存
void vma_cache_flush(void) {
    vma_cache_cleanup();
    vma_cache_init(global_vma_cache.max_count);
}

// 获取缓存统计信息
void vma_cache_get_stats(int* hits, int* misses, int* evictions) {
    

    *hits = global_vma_cache.current_count;
    *misses = 0;  // 实际应该通过访问跟踪实现
    *evictions = 0;

    
}

// 初始化全局VMA缓存
__attribute__((constructor)) static void init_vma_cache(void) {
    vma_cache_init(VMA_CACHE_SIZE);
}

// 清理全局VMA缓存
__attribute__((destructor)) static void cleanup_vma_cache(void) {
    vma_cache_cleanup();
}