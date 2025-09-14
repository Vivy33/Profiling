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

#define PATH_MAX 4096

static char* get_elf_build_id(const char* filename);
static void remove_elf(struct elf_file_cache* elf_table, const char* build_id);

// 辅助函数：通过build_id在哈希表中查找ELF文件
static struct elf_file* find_elf_in_table(struct elf_file_cache* elf_table, const char* build_id) {
    unsigned int index = hash_str(build_id);
    struct elf_file_hash_node* node = elf_table->cache_buckets[index];
    while (node) {
        if (strcmp(node->elf_file_data.build_id, build_id) == 0) {
            return &node->elf_file_data;
        }
        node = node->next_node;
    }
    return NULL;
}

// 辅助函数：将新的ELF文件插入哈希表
static void insert_elf_into_table(struct elf_file_cache* elf_table, struct elf_file_hash_node* new_node) {
    unsigned int index = hash_str(new_node->elf_file_data.build_id);
    new_node->next_node = elf_table->cache_buckets[index];
    elf_table->cache_buckets[index] = new_node;
}

/**
 * @brief 查找或创建ELF文件对象
 * @param sys 指向system_context结构体的指针，包含ELF文件缓存。
 * @param filename ELF文件的路径。
 * @return 成功时返回指向elf_file结构体的指针，失败时返回NULL。
 */
struct elf_file* find_or_create_elf(struct system_context* sys, const char *filename) {
    if (!filename || filename[0] == '\0' || filename[0] == '[') {
        return NULL; // 忽略匿名内存区域或无效名称
    }

    // 对于系统范围的性能分析，直接使用文件名
    char host_path[PATH_MAX];
    snprintf(host_path, sizeof(host_path), "%s", filename);

    char* build_id = get_elf_build_id(host_path);
    if (!build_id) {
        // 对于宿主进程或/proc/<pid>/root不可访问的情况，回退到原始路径
        build_id = get_elf_build_id(filename);
        if (!build_id) {
            return NULL;
        }
    }

    struct elf_file* elf_obj = find_elf_in_table(sys->elf_cache, build_id);
    if (elf_obj) {
        elf_obj->reference_count++;
        free(build_id); // 不再需要
        return elf_obj;
    }

    // 未找到ELF，创建一个新的
    struct elf_file_hash_node* new_node = calloc(1, sizeof(struct elf_file_hash_node));
    if (!new_node) {
        perror("为new_node分配内存失败");
        free(build_id);
        return NULL;
    }

    new_node->elf_file_data.build_id = build_id; // 所有权转移
    new_node->elf_file_data.file_path = strdup(filename); // 存储原始路径
    if (!new_node->elf_file_data.file_path) {
        perror("为file_path复制字符串失败");
        free(new_node->elf_file_data.build_id);
        free(new_node);
        return NULL;
    }

    // 使用宿主路径解析ELF符号
    new_node->elf_file_data.symbols = get_elf_func_symbols(host_path, &new_node->elf_file_data);
    if (!new_node->elf_file_data.symbols) {
        // 宿主进程的回退
        new_node->elf_file_data.symbols = get_elf_func_symbols(filename, &new_node->elf_file_data);
        if (!new_node->elf_file_data.symbols) {
            free(new_node->elf_file_data.file_path);
            free(new_node->elf_file_data.build_id);
            free(new_node);
            return NULL;
        }
    }
    
    new_node->elf_file_data.reference_count = 1;
    insert_elf_into_table(sys->elf_cache, new_node);

    return &new_node->elf_file_data;
}

/**
 * @brief 减少ELF文件的引用计数。如果引用计数归零，则从缓存中移除该文件。
 * @param elf_table ELF文件缓存表指针
 * @param filename ELF文件的路径
 */
void release_elf(struct elf_file_cache* elf_table, const char* filename) {
    if (!filename || !elf_table) return;

    char* build_id = get_elf_build_id(filename);
    if (!build_id) {
        return; // 无法在没有build_id的情况下找到它
    }

    struct elf_file* elf_obj = find_elf_in_table(elf_table, build_id);
    if (elf_obj) {
        elf_obj->reference_count--;
        if (elf_obj->reference_count == 0) {
            remove_elf(elf_table, build_id);
        }
    }
    free(build_id);
}

/**
 * @brief 从哈希表中移除一个ELF对象并释放其资源。
 * @param elf_table ELF文件缓存表指针
 * @param build_id ELF文件的Build ID
 */
static void remove_elf(struct elf_file_cache* elf_table, const char* build_id) {
    unsigned int index = hash_str(build_id);
    struct elf_file_hash_node* node = elf_table->cache_buckets[index];
    struct elf_file_hash_node* prev = NULL;

    while (node) {
        if (strcmp(node->elf_file_data.build_id, build_id) == 0) {
            // 解除节点链接
            if (prev) {
                prev->next_node = node->next_node;
            } else {
                elf_table->cache_buckets[index] = node->next_node;
            }

            // 释放内部数据
            free(node->elf_file_data.build_id);
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


/**
 * @brief 清空整个ELF缓存，释放所有相关内存
 * @param elf_table ELF文件缓存表指针
 */
void clear_elf_cache(struct elf_file_cache* elf_table) {
    if (!elf_table) return;

    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        // 注意: remove_elf会修改列表，所以我们需要小心处理
        while (elf_table->cache_buckets[i] != NULL) {
            // remove_elf会找到第一个节点，释放它，并重新链接头部
            remove_elf(elf_table, elf_table->cache_buckets[i]->elf_file_data.build_id);
        }
    }
}

/**
 * @brief 从ELF文件中提取GNU build ID。
 * @param filename ELF文件的路径。
 * @return 堆分配的build ID十六进制字符串，如果未找到或出错则返回NULL。
 *         调用者负责释放返回的字符串。
 */
static char* get_elf_build_id(const char* filename) {
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

    size_t shstrndx;
    if (elf_getshdrstrndx(e, &shstrndx) != 0) {
        elf_end(e);
        close(fd);
        return NULL;
    }

    char* build_id_str = NULL;
    Elf_Scn* scn = NULL;
    GElf_Shdr shdr;
    while ((scn = elf_nextscn(e, scn)) != NULL) {
        gelf_getshdr(scn, &shdr);
        char* name = elf_strptr(e, shstrndx, shdr.sh_name);
        if (name && strcmp(name, ".note.gnu.build-id") == 0 && shdr.sh_type == SHT_NOTE) {
            Elf_Data* data = elf_getdata(scn, NULL);
            if (data && data->d_buf) {
                GElf_Nhdr nhdr;
                size_t offset = 0;
                while (offset + sizeof(GElf_Nhdr) < data->d_size) {
                    if (!gelf_getnote(data, offset, &nhdr, NULL, NULL)) {
                        break;
                    }
                    size_t name_sz_aligned = (nhdr.n_namesz + 3) & ~3;
                    size_t desc_sz_aligned = (nhdr.n_descsz + 3) & ~3;
                    
                    if (nhdr.n_type == NT_GNU_BUILD_ID && nhdr.n_descsz > 0) {
                        unsigned char* build_id_raw = (unsigned char*)data->d_buf + offset + sizeof(GElf_Nhdr) + name_sz_aligned;
                        build_id_str = malloc(nhdr.n_descsz * 2 + 1);
                        if (build_id_str) {
                            for (size_t i = 0; i < nhdr.n_descsz; i++) {
                                sprintf(build_id_str + i * 2, "%02x", build_id_raw[i]);
                            }
                            build_id_str[nhdr.n_descsz * 2] = '\0';
                        }
                        goto end_loop;
                    }
                    offset += sizeof(GElf_Nhdr) + name_sz_aligned + desc_sz_aligned;
                }
            }
        }
    }
end_loop:
    elf_end(e);
    close(fd);
    return build_id_str;
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