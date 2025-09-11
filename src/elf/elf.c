/**
 * @file elf.c
 * @brief ELF文件解析与缓存管理核心模块
 * 
 * 负责ELF文件的解析、缓存和生命周期管理。
 * 实现了ELF文件的高效缓存机制，避免重复的文件I/O和解析开销。
 * 
 * 核心功能：
 * - ELF文件解析：使用libelf库解析ELF格式
 * - 符号提取：提取函数符号并构建红黑树索引
 * - 缓存管理：基于引用计数的缓存生命周期管理
 * - 内存优化：自动清理未引用的ELF文件
 * 
 * 数据结构：
 * - elf_file: 单个ELF文件的完整表示
 * - elf_file_cache: ELF文件哈希表，文件名→ELF信息
 * - elf_symbol_collection: 符号集合，使用红黑树管理
 * 
 * 缓存策略：
 * - 惰性加载：按需解析ELF文件
 * - 引用计数：0引用时自动清理
 * - 哈希表：O(1)平均时间复杂度查找
 */

#include <libelf.h>
#include <gelf.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/header.h"
#include "elf_utils.h"

/**
 * @brief 从ELF缓存中移除指定文件
 * @param elf_table ELF文件缓存表
 * @param filename 要移除的文件名
 * 
 * 内部辅助函数，执行实际的ELF文件移除操作：
 * - 从哈希表中移除节点
 * - 释放所有相关内存（文件路径、符号表、ELF结构）
 * - 处理哈希冲突链表
 */
static void remove_elf(struct elf_file_cache* elf_table, const char* filename);

// Helper to search for an ELF file in the hash table
static struct elf_file* find_elf_in_table(struct elf_file_cache* elf_table, const char* filename) {
    unsigned int index = hash_str(filename);
    struct elf_file_hash_node* node = elf_table->cache_buckets[index];
    while (node) {
        if (strcmp(node->elf_file_data.file_path, filename) == 0) {
            return &node->elf_file_data;
        }
        node = node->next_node;
    }
    return NULL;
}

// Helper to insert a new ELF file into the hash table
static void insert_elf_into_table(struct elf_file_cache* elf_table, struct elf_file_hash_node* new_node) {
    unsigned int index = hash_str(new_node->elf_file_data.file_path);
    new_node->next_node = elf_table->cache_buckets[index];
    elf_table->cache_buckets[index] = new_node;
}

// Find an existing ELF object or create a new one if not found.
struct elf_file* find_or_create_elf(struct system_context* sys, const char *filename) {
    if (!filename || filename[0] == '\0' || filename[0] == '[') {
        return NULL; // Ignore anonymous memory regions or invalid names
    }

    struct elf_file* elf_obj = find_elf_in_table(sys->elf_cache, filename);
    if (elf_obj) {
        elf_obj->reference_count++;
        return elf_obj;
    }

    // ELF not found, create a new one
    struct elf_file_hash_node* new_node = calloc(1, sizeof(struct elf_file_hash_node));
    if (!new_node) {
        perror("calloc for new_node failed");
        return NULL;
    }

    new_node->elf_file_data.file_path = strdup(filename);
    if (!new_node->elf_file_data.file_path) {
        perror("strdup for file_path failed");
        free(new_node);
        return NULL;
    }

    // Parse ELF symbols
    new_node->elf_file_data.symbols = get_elf_func_symbols(filename, &new_node->elf_file_data);
    if (!new_node->elf_file_data.symbols) {
        free(new_node->elf_file_data.file_path);
        free(new_node);
        return NULL;
    }
    
    new_node->elf_file_data.reference_count = 1;
    insert_elf_into_table(sys->elf_cache, new_node);

    return &new_node->elf_file_data;
}

// Decrement the reference count for an ELF file. If it reaches zero, remove it.
void release_elf(struct elf_file_cache* elf_table, const char* filename) {
    if (!filename || !elf_table) return;

    struct elf_file* elf_obj = find_elf_in_table(elf_table, filename);
    if (elf_obj) {
        elf_obj->reference_count--;
        if (elf_obj->reference_count == 0) {
            remove_elf(elf_table, filename);
        }
    }
}

// Remove an ELF object from the hash table and free its resources.
static void remove_elf(struct elf_file_cache* elf_table, const char* filename) {
    unsigned int index = hash_str(filename);
    struct elf_file_hash_node* node = elf_table->cache_buckets[index];
    struct elf_file_hash_node* prev = NULL;

    while (node) {
        if (strcmp(node->elf_file_data.file_path, filename) == 0) {
            // Unlink the node
            if (prev) {
                prev->next_node = node->next_node;
            } else {
                elf_table->cache_buckets[index] = node->next_node;
            }

            // Free internal data
            free(node->elf_file_data.file_path);
            if (node->elf_file_data.symbols) {
                struct rb_root* root = &node->elf_file_data.symbols->symbol_tree;
                struct rb_node* rb_node = rb_first(root);
                while (rb_node) {
                    struct symbol_info* sym = rb_entry(rb_node, struct symbol_info, symbol_rb_node);
                    struct rb_node* next_node = rb_next(rb_node);
                    rb_erase(rb_node, root);
                    free(sym->symbol_name);
                    free(sym);
                    rb_node = next_node;
                }
                free(node->elf_file_data.symbols);
            }
            free(node);
            return;
        }
        prev = node;
        node = node->next_node;
    }
}


// Clear the entire ELF cache, freeing all associated memory
void clear_elf_cache(struct elf_file_cache* elf_table) {
    if (!elf_table) return;

    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        // Note: remove_elf modifies the list, so we need to be careful
        while (elf_table->cache_buckets[i] != NULL) {
            // remove_elf will find the first node, free it, and relink the head
            remove_elf(elf_table, elf_table->cache_buckets[i]->elf_file_data.file_path);
        }
    }
}

/**
 * @brief 解析ELF文件中的函数符号
 * @param filename ELF文件路径
 * @param elf_info ELF文件结构指针（用于存储解析结果）
 * @return elf_symbol_collection* 符号集合，NULL表示失败
 * 
 * 符号解析流程：
 * 1. 打开ELF文件并验证格式
 * 2. 遍历所有section，查找符号表(.symtab)和动态符号表(.dynsym)
 * 3. 提取函数符号（STT_FUNC类型）
 * 4. 构建红黑树索引，按地址排序
 * 5. 返回符号集合
 * 
 * 符号类型：
 * - STT_FUNC: 函数符号
 * - STT_OBJECT: 数据对象符号
 * - 只处理STT_FUNC类型，因为性能分析主要关注函数调用
 * 
 * 内存管理：
 * - 使用strdup复制符号名称
 * - 使用红黑树管理符号，支持快速查找
 * - 失败时自动清理已分配内存
 */
struct elf_symbol_collection* get_elf_func_symbols(const char* filename, struct elf_file* elf_info) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return NULL;

    if (elf_version(EV_CURRENT) == EV_NONE) {
        close(fd);
        return NULL;
    }

    Elf* e = elf_begin(fd, ELF_C_READ, NULL);
    if (!e) {
        close(fd);
        return NULL;
    }

    struct elf_symbol_collection* symbols = calloc(1, sizeof(struct elf_symbol_collection));
    if (!symbols) {
        elf_end(e);
        close(fd);
        return NULL;
    }
    symbols->symbol_tree = RB_ROOT;

    Elf_Scn* scn = NULL;
    GElf_Shdr shdr;
    while ((scn = elf_nextscn(e, scn)) != NULL) {
        gelf_getshdr(scn, &shdr);

        if (shdr.sh_type == SHT_SYMTAB || shdr.sh_type == SHT_DYNSYM) {
            Elf_Data* data = elf_getdata(scn, NULL);
            size_t count = shdr.sh_size / shdr.sh_entsize;

            for (size_t i = 0; i < count; ++i) {
                GElf_Sym sym;
                gelf_getsym(data, i, &sym);

                if (GELF_ST_TYPE(sym.st_info) == STT_FUNC && sym.st_size > 0) {
                    struct symbol_info* new_sym = calloc(1, sizeof(struct symbol_info));
                    if (!new_sym) continue;

                    new_sym->symbol_name = strdup(elf_strptr(e, shdr.sh_link, sym.st_name));
                    new_sym->symbol_start = sym.st_value;
                    new_sym->symbol_size = sym.st_size;

                    struct rb_node** link = &symbols->symbol_tree.rb_node;
                    struct rb_node* parent = NULL;
                    struct symbol_info* entry;

                    while (*link) {
                        parent = *link;
                        entry = rb_entry(parent, struct symbol_info, symbol_rb_node);
                        if (new_sym->symbol_start < entry->symbol_start) {
                            link = &(*link)->rb_left;
                        } else {
                            link = &(*link)->rb_right;
                        }
                    }
                    rb_link_node(&new_sym->symbol_rb_node, parent, link);
                    rb_insert_color(&new_sym->symbol_rb_node, &symbols->symbol_tree);
                    symbols->total_symbols++;
                }
            }
        }
    }

    elf_end(e);
    close(fd);

    if (symbols->total_symbols == 0) {
        free(symbols);
        return NULL;
    }

    return symbols;
}