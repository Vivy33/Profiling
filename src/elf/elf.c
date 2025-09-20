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

// 静态辅助函数前向声明
static void remove_elf(struct elf_file_cache* elf_table, const char* build_id);
static struct elf_symbol_collection* build_symbol_collection(Elf* e, struct elf_file* elf_info);
static char* get_elf_build_id_from_elf(Elf* e);
static Elf* elf_parser_init(const char* filename, int* out_fd);
static void elf_parser_cleanup(Elf* e, int fd);


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
 * @brief (重构后) 查找或创建ELF文件对象，确保每个文件只被解析一次。
 * 
 * 此函数是ELF缓存机制的核心。它首先尝试通过Build ID在缓存中查找ELF文件。
 * 如果找到，则增加其引用计数并返回。
 * 如果未找到，它将执行以下高效流程：
 *   1. 调用 elf_parser_init() 打开文件并获取libelf句柄。
 *   2. 使用此句柄，一次性提取Build ID和所有函数符号。
 *   3. 创建一个新的elf_file对象，存入缓存并初始化引用计数为1。
 *   4. 调用 elf_parser_cleanup() 清理资源。
 * 这种方法避免了对同一文件的重复读取和解析，显著提升了性能。
 *
 * @param sys 指向system_context结构体的指针，包含ELF文件缓存。
 * @param filename ELF文件的路径。
 * @return 成功时返回指向elf_file结构体的指针，失败时返回NULL。
 */
struct elf_file* find_or_create_elf(struct system_context* sys, int pid, const char *filename) {
    if (!filename || filename[0] != '/') {
        return NULL; // 忽略匿名内存区域或无效名称
    }

    char host_path[PATH_MAX];
    snprintf(host_path, sizeof(host_path), "/proc/%d/root/%s", pid, filename);

    // 修正：移除多余的斜杠，如果filename已经是绝对路径
    if (filename[0] == '/') {
        snprintf(host_path, sizeof(host_path), "/proc/%d/root%s", pid, filename);
    } else {
        // 这是针对非绝对路径（如vdso）的修正
        snprintf(host_path, sizeof(host_path), "/proc/%d/root/%s", pid, filename);
    }

    int fd = -1;
    Elf* e = elf_parser_init(host_path, &fd);
    if (!e) {
        // 对于宿主进程或/proc/<pid>/root不可访问的情况，回退到原始路径
        e = elf_parser_init(filename, &fd);
        if (!e) return NULL;
    }

    char* build_id = get_elf_build_id_from_elf(e);
    if (!build_id) {
        elf_parser_cleanup(e, fd);
        return NULL;
    }

    struct elf_file* elf_obj = find_elf_in_table(sys->elf_cache, build_id);
    if (elf_obj) {
        elf_obj->reference_count++;
        free(build_id);
        elf_parser_cleanup(e, fd);
        return elf_obj;
    }

    // 未找到ELF，创建一个新的
    struct elf_file_hash_node* new_node = (struct elf_file_hash_node*)calloc(1, sizeof(struct elf_file_hash_node));
    if (!new_node) {
        perror("为new_node分配内存失败");
        free(build_id);
        elf_parser_cleanup(e, fd);
        return NULL;
    }

    new_node->elf_file_data.build_id = build_id; // 所有权转移
    new_node->elf_file_data.file_path = strdup(filename); // 存储原始路径
    if (!new_node->elf_file_data.file_path) {
        perror("为file_path复制字符串失败");
        free(new_node->elf_file_data.build_id);
        free(new_node);
        elf_parser_cleanup(e, fd);
        return NULL;
    }

    // 从同一个ELF句柄获取符号
    new_node->elf_file_data.symbols = build_symbol_collection(e, &new_node->elf_file_data);
    if (!new_node->elf_file_data.symbols) {
        free(new_node->elf_file_data.file_path);
        free(new_node->elf_file_data.build_id);
        free(new_node);
        elf_parser_cleanup(e, fd);
        return NULL;
    }
    
    new_node->elf_file_data.reference_count = 1;
    insert_elf_into_table(sys->elf_cache, new_node);

    elf_parser_cleanup(e, fd); // 解析成功后清理
    return &new_node->elf_file_data;
}


/**
 * @brief (重构后) 通过指针减少ELF文件的引用计数。如果引用计数归零，则从缓存中移除。
 * 
 * 这是新的、更安全的ELF释放机制。它直接操作elf_file对象，避免了旧版release_elf中
 * 不安全且低效的文件重读操作。
 *
 * @param elf_table ELF文件缓存表指针。
 * @param elf_obj 指向要释放的elf_file对象的指针。
 */
void release_elf_by_ptr(struct elf_file_cache* elf_table, struct elf_file* elf_obj) {
    if (!elf_obj) return;

    elf_obj->reference_count--;
    if (elf_obj->reference_count == 0) {
        remove_elf(elf_table, elf_obj->build_id);
    }
}

/**
 * @brief (内部辅助函数) 释放单个elf_file结构体内部的所有动态分配的资源。
 *
 * @param elf_data 指向要释放资源的elf_file结构体的指针。
 */
static void free_elf_resources(struct elf_file* elf_data) {
    if (!elf_data) return;

    free(elf_data->build_id);
    free(elf_data->file_path);

    if (elf_data->symbols) {
        struct rb_root* root = &elf_data->symbols->symbol_tree;
        struct rb_node* rb_node = rb_first(root);
        while (rb_node) {
            struct symbol_info* sym = rb_entry(rb_node, struct symbol_info, symbol_rb_node);
            struct rb_node* next_node = rb_next(rb_node);
            rb_erase(rb_node, root);
            free(sym->symbol_name);
            free(sym);
            rb_node = next_node;
        }
        free(elf_data->symbols);
    }
}

/**
 * @brief 从哈希表中移除一个ELF对象并释放其资源。
 * 
 * (重构后) 此函数现在将资源释放的复杂性委托给了 free_elf_resources 辅助函数，
 * 使其自身逻辑更专注于节点的查找和链表操作。
 *
 * @param elf_table ELF文件缓存表指针
 * @param build_id ELF文件的Build ID
 */
static void remove_elf(struct elf_file_cache* elf_table, const char* build_id) {
    unsigned int index = hash_str(build_id);
    struct elf_file_hash_node* node = elf_table->cache_buckets[index];
    struct elf_file_hash_node* prev = NULL;

    while (node) {
        if (strcmp(node->elf_file_data.build_id, build_id) == 0) {
            // 从链表中解除节点链接
            if (prev) {
                prev->next_node = node->next_node;
            } else {
                elf_table->cache_buckets[index] = node->next_node;
            }

            // 释放节点内部的所有资源
            free_elf_resources(&node->elf_file_data);
            
            // 释放节点本身
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

// =================================================================
// 新增静态辅助函数 (重构核心)
// =================================================================

/**
 * @brief 初始化ELF解析流程，负责打开文件并创建libelf句柄。
 * 
 * 这是ELF文件处理的第一步。它会打开指定路径的文件，并初始化libelf库，
 * 为后续的Build ID提取和符号解析做准备。
 *
 * @param filename 要打开和解析的ELF文件的完整路径。
 * @param out_fd 一个整型指针，用于传出打开文件的文件描述符(fd)。
 *               这样做的目的是为了让调用者能够在完成所有操作后关闭它。
 * @return 成功时返回一个指向Elf结构的指针 (libelf句柄)，失败时返回NULL。
 */
static Elf* elf_parser_init(const char* filename, int* out_fd) {
    *out_fd = open(filename, O_RDONLY);
    if (*out_fd < 0) return NULL;

    Elf* e = elf_begin(*out_fd, ELF_C_READ, NULL);
    if (!e) {
        close(*out_fd);
        *out_fd = -1;
        return NULL;
    }
    return e;
}

/**
 * @brief 清理并关闭由elf_parser_init创建的资源。
 *
 * @param e 要终止的libelf会话句柄。
 * @param fd 要关闭的文件描述符。
 */
static void elf_parser_cleanup(Elf* e, int fd) {
    if (e) elf_end(e);
    if (fd >= 0) close(fd);
}

/**
 * @brief 从一个已打开的libelf句柄中提取GNU Build ID。
 * 
 * 此函数遍历ELF文件的所有节(sections)，专门查找名为 ".note.gnu.build-id" 的节。
 * 找到后，它会解析这个NOTE类型的节，提取出十六进制的Build ID。
 *
 * @param e 一个已初始化的有效libelf句柄。
 * @return 成功时返回一个动态分配的、包含Build ID的字符串。调用者负责释放此内存。
 *         如果未找到Build ID或发生错误，则返回NULL。
 */
static char* get_elf_build_id_from_elf(Elf* e) {
    size_t shstrndx;
    if (elf_getshdrstrndx(e, &shstrndx) != 0) {
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
                    size_t name_offset, desc_offset;
                    if (!gelf_getnote(data, offset, &nhdr, &name_offset, &desc_offset)) {
                        break;
                    }

                    // 关键修复 #2: 健全性检查，防止整数溢出
                    if (nhdr.n_namesz > data->d_size || nhdr.n_descsz > data->d_size) {
                        break; // 畸形的note，尺寸值不合理
                    }

                    size_t name_sz_aligned = (nhdr.n_namesz + 3) & ~3;
                    size_t desc_sz_aligned = (nhdr.n_descsz + 3) & ~3;
                    
                    // 关键修复 #1: 在处理之前，检查整个note是否在边界内
                    if (offset + sizeof(GElf_Nhdr) + name_sz_aligned + desc_sz_aligned > data->d_size) {
                        break; // 畸形的note，尺寸超出节区边界
                    }

                    if (nhdr.n_type == NT_GNU_BUILD_ID && nhdr.n_descsz > 0) {
                        unsigned char* build_id_raw = (unsigned char*)data->d_buf + offset + sizeof(GElf_Nhdr) + name_sz_aligned;
                        build_id_str = (char*)malloc(nhdr.n_descsz * 2 + 1);
                        if (build_id_str) {
                            for (size_t i = 0; i < nhdr.n_descsz; i++) {
                                sprintf(build_id_str + i * 2, "%02x", build_id_raw[i]);
                            }
                            build_id_str[nhdr.n_descsz * 2] = '\0';
                        }
                        return build_id_str; // 找到后立即返回
                    }
                    offset += sizeof(GElf_Nhdr) + name_sz_aligned + desc_sz_aligned;
                }
            }
        }
    }
    return NULL; // 未找到
}

/**
 * @brief (内部辅助函数) 将单个符号信息插入到红黑树中。
 *
 * @param root 指向符号红黑树根节点的指针。
 * @param new_sym 要插入的新符号。
 */
static void insert_symbol_into_tree(struct rb_root* root, struct symbol_info* new_sym) {
    struct rb_node** link = &root->rb_node;
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
    rb_insert_color(&new_sym->symbol_rb_node, root);
}

/**
 * @brief (内部辅助函数) 处理单个符号表节（.symtab 或 .dynsym）。
 *
 * @param e libelf句柄。
 * @param scn 当前ELF节区。
 * @param shdr 当前节区的头部信息。
 * @param symbols 指向符号集合的指针，用于存储找到的符号。
 */
static void process_symbol_section(Elf* e, Elf_Scn* scn, GElf_Shdr* shdr, struct elf_symbol_collection* symbols) {
    Elf_Data* data = elf_getdata(scn, NULL);
    if (!data) return;

    size_t count = shdr->sh_size / shdr->sh_entsize;
    for (size_t i = 0; i < count; ++i) {
        GElf_Sym sym;
        gelf_getsym(data, i, &sym);

        if (GELF_ST_TYPE(sym.st_info) == STT_FUNC && sym.st_size > 0) {
            struct symbol_info* new_sym = (struct symbol_info*)calloc(1, sizeof(struct symbol_info));
            if (!new_sym) continue;

            new_sym->symbol_name = strdup(elf_strptr(e, shdr->sh_link, sym.st_name));
            new_sym->symbol_start = sym.st_value;
            new_sym->symbol_size = sym.st_size;

            insert_symbol_into_tree(&symbols->symbol_tree, new_sym);
            symbols->total_symbols++;
        }
    }
}

/**
 * @brief 从一个已打开的libelf句柄中解析所有函数符号。
 * 
 * (重构后) 此函数通过遍历ELF节区来查找符号表，然后委托给
 * process_symbol_section 函数来处理每个具体的符号表。
 *
 * @param e 一个已初始化的有效libelf句柄。
 * @param elf_info 指向elf_file结构体的指针，此参数当前未使用，但为未来扩展保留。
 * @return 成功时返回一个指向 elf_symbol_collection 的指针，其中包含了按地址排序的符号红黑树。
 *         如果解析失败或未找到任何函数符号，则返回NULL。
 */
static struct elf_symbol_collection* build_symbol_collection(Elf* e, struct elf_file* elf_info) {
    struct elf_symbol_collection* symbols = (struct elf_symbol_collection*)calloc(1, sizeof(struct elf_symbol_collection));
    if (!symbols) {
        return NULL;
    }
    symbols->symbol_tree = RB_ROOT;

    Elf_Scn* scn = NULL;
    GElf_Shdr shdr;
    while ((scn = elf_nextscn(e, scn)) != NULL) {
        gelf_getshdr(scn, &shdr);
        if (shdr.sh_type == SHT_SYMTAB || shdr.sh_type == SHT_DYNSYM) {
            process_symbol_section(e, scn, &shdr, symbols);
        }
    }

    if (symbols->total_symbols == 0) {
        free(symbols);
        return NULL;
    }

    return symbols;
}