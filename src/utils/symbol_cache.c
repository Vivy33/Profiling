#include "symbol_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// 全局符号缓存管理器
static struct symbol_cache_manager global_symbol_cache = {0};

// 初始化符号缓存
int symbol_cache_init(int max_symbols) {
    global_symbol_cache.symbol_tree = RB_ROOT;
    global_symbol_cache.block_tree = RB_ROOT;
    INIT_LIST_HEAD(&global_symbol_cache.lru_list);
    global_symbol_cache.current_symbols = 0;
    global_symbol_cache.max_symbols = max_symbols;
    global_symbol_cache.access_counter = 0;

    return 0;
}

// 清理符号缓存
void symbol_cache_cleanup(void) {

    // 清理符号树
    struct rb_node* node = rb_first(&global_symbol_cache.symbol_tree);
    while (node) {
        struct symbol_cache_entry* entry = rb_entry(node, struct symbol_cache_entry, symbol_rb_node);
        struct rb_node* next = rb_next(node);
        rb_erase(node, &global_symbol_cache.symbol_tree);
        free(entry->name);
        free(entry->filename);
        free(entry);
        node = next;
    }

    // 清理符号块树
    node = rb_first(&global_symbol_cache.block_tree);
    while (node) {
        struct symbol_cache_block* block = rb_entry(node, struct symbol_cache_block, block_rb_node);
        struct rb_node* next = rb_next(node);
        rb_erase(node, &global_symbol_cache.block_tree);

        // 清理块内的符号条目
        for (int i = 0; i < block->entry_count; i++) {
            free(block->entries[i].name);
            free(block->entries[i].filename);
        }

        free(block);
        node = next;
    }

    // 清理LRU链表
    struct list_head* pos, *tmp;
    list_for_each_safe(pos, tmp, &global_symbol_cache.lru_list) {
        struct symbol_cache_entry* entry = list_entry(pos, struct symbol_cache_entry, lru_list);
        list_del(pos);
        free(entry->name);
        free(entry->filename);
        free(entry);
    }

    global_symbol_cache.current_symbols = 0;
    
}

// 查找符号缓存条目（RB树查找）
static struct symbol_cache_entry* find_symbol_entry(uint64_t addr) {
    struct rb_node* node = global_symbol_cache.symbol_tree.rb_node;

    while (node) {
        struct symbol_cache_entry* entry = rb_entry(node, struct symbol_cache_entry, symbol_rb_node);

        if (addr < entry->addr) {
            node = node->rb_left;
        } else if (addr >= entry->addr + entry->size) {
            node = node->rb_right;
        } else {
            return entry;
        }
    }

    return NULL;
}

// 查找符号块（RB树查找）
static struct symbol_cache_block* find_symbol_block(uint64_t addr) {
    struct rb_node* node = global_symbol_cache.block_tree.rb_node;

    while (node) {
        struct symbol_cache_block* block = rb_entry(node, struct symbol_cache_block, block_rb_node);

        if (addr < block->start_addr) {
            node = node->rb_left;
        } else if (addr >= block->end_addr) {
            node = node->rb_right;
        } else {
            return block;
        }
    }

    return NULL;
}

// 淘汰最老的符号（LRU策略）
static void evict_lru_symbol(void) {
    if (list_empty(&global_symbol_cache.lru_list)) {
        return;
    }

    struct symbol_cache_entry* lru_entry = list_first_entry(
        &global_symbol_cache.lru_list, struct symbol_cache_entry, lru_list);

    rb_erase(&lru_entry->symbol_rb_node, &global_symbol_cache.symbol_tree);
    list_del(&lru_entry->lru_list);
    free(lru_entry->name);
    free(lru_entry->filename);
    free(lru_entry);
    global_symbol_cache.current_symbols--;
}

// 添加符号到缓存
int symbol_cache_add(const char* name, uint64_t addr, uint64_t size, const char* filename) {
    if (!name || !filename) return -1;


    // 检查是否已存在
    struct symbol_cache_entry* existing = find_symbol_entry(addr);
    if (existing) {
            return 0;
    }

    // 如果缓存满了，淘汰最老的
    if (global_symbol_cache.current_symbols >= global_symbol_cache.max_symbols) {
        evict_lru_symbol();
    }

    struct symbol_cache_entry* entry = calloc(1, sizeof(struct symbol_cache_entry));
    if (!entry) {
            return -1;
    }

    entry->addr = addr;
    entry->size = size;
    entry->last_access = ++global_symbol_cache.access_counter;
    entry->ref_count = 1;
    INIT_LIST_HEAD(&entry->lru_list);

    entry->name = strdup(name);
    entry->filename = strdup(filename);
    if (!entry->name || !entry->filename) {
        free(entry->name);
        free(entry->filename);
        free(entry);
            return -1;
    }

    // 插入RB树
    struct rb_node **new = &global_symbol_cache.symbol_tree.rb_node, *parent = NULL;
    while (*new) {
        struct symbol_cache_entry* this = rb_entry(*new, struct symbol_cache_entry, symbol_rb_node);
        parent = *new;

        if (addr < this->addr) {
            new = &((*new)->rb_left);
        } else {
            new = &((*new)->rb_right);
        }
    }

    rb_link_node(&entry->symbol_rb_node, parent, new);
    rb_insert_color(&entry->symbol_rb_node, &global_symbol_cache.symbol_tree);

    // 添加到LRU链表
    list_add_tail(&entry->lru_list, &global_symbol_cache.lru_list);
    global_symbol_cache.current_symbols++;

    return 0;
}

// 批量添加符号块
int symbol_cache_add_block(struct symbol_info* symbols, int count, const char* filename) {
    if (!symbols || !filename || count <= 0) return -1;


    // 计算符号块的范围
    uint64_t min_addr = symbols[0].symbol_start;
    uint64_t max_addr = symbols[0].symbol_start;
    for (int i = 1; i < count; i++) {
        if (symbols[i].symbol_start < min_addr) min_addr = symbols[i].symbol_start;
        if (symbols[i].symbol_start + symbols[i].symbol_size > max_addr) {
            max_addr = symbols[i].symbol_start + symbols[i].symbol_size;
        }
    }

    // 检查是否已存在该块
    struct symbol_cache_block* existing = find_symbol_block(min_addr);
    if (existing) {
            return 0;
    }

    // 批量添加符号
    for (int i = 0; i < count; i++) {
        symbol_cache_add(symbols[i].symbol_name, symbols[i].symbol_start,
                        symbols[i].symbol_size, filename);
    }

    return 0;
}

// 查找符号缓存
struct symbol_cache_entry* symbol_cache_lookup(uint64_t addr) {

    struct symbol_cache_entry* entry = find_symbol_entry(addr);
    if (entry) {
        entry->last_access = ++global_symbol_cache.access_counter;
        entry->ref_count++;

        // 移动到LRU链表尾部
        list_del(&entry->lru_list);
        list_add_tail(&entry->lru_list, &global_symbol_cache.lru_list);
    }

    return entry;
}

// 移除符号缓存
void symbol_cache_remove(uint64_t addr) {

    struct symbol_cache_entry* entry = find_symbol_entry(addr);
    if (entry) {
        rb_erase(&entry->symbol_rb_node, &global_symbol_cache.symbol_tree);
        list_del(&entry->lru_list);
        free(entry->name);
        free(entry->filename);
        free(entry);
        global_symbol_cache.current_symbols--;
    }

}

// 释放符号缓存条目
void symbol_cache_put(struct symbol_cache_entry* entry) {
    if (!entry) return;

    entry->ref_count--;
}

// 获取符号名称（带缓存优化）
const char* symbol_cache_get_name(uint64_t addr, const char* filename) {
    struct symbol_cache_entry* entry = symbol_cache_lookup(addr);
    if (entry) {
        if (strcmp(entry->filename, filename) == 0) {
            const char* name = entry->name;
            symbol_cache_put(entry);
            return name;
        }
        symbol_cache_put(entry);
    }

    return NULL;
}

// 延迟加载符号表
int symbol_cache_lazy_load(const char* filename, uint64_t addr) {
    if (!filename) return -1;

    // 检查是否已加载该文件的符号

    // 简化的延迟加载：这里应该实际加载ELF符号
    // 实际实现中应该解析ELF文件并加载相关符号

    return 0;
}

// 强制刷新符号缓存
void symbol_cache_flush(void) {
    symbol_cache_cleanup();
    symbol_cache_init(SYMBOL_CACHE_SIZE);
}

// 获取缓存统计信息
void symbol_cache_get_stats(int* hits, int* misses, int* evictions) {

    *hits = global_symbol_cache.current_symbols;
    *misses = 0;  // 实际应该通过访问跟踪实现
    *evictions = 0;

}

// 初始化全局符号缓存
__attribute__((constructor)) static void init_symbol_cache(void) {
    symbol_cache_init(SYMBOL_CACHE_SIZE);
}

// 清理全局符号缓存
__attribute__((destructor)) static void cleanup_symbol_cache(void) {
    symbol_cache_cleanup();
}