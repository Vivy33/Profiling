/**
 * @file symbol_table.c
 * @brief ELF符号表解析和查找核心模块
 * 
 * 负责ELF文件中符号信息的提取、管理和高效查找，包括：
 * - ELF符号表的完整解析（.symtab和.dynsym）
 * - 函数符号的筛选和提取（STT_FUNC类型）
 * - 符号信息的红黑树索引构建
 * - 基于地址的符号快速查找（O(log n)复杂度）
 * - 符号名称的安全返回和错误处理
 * 
 * 核心数据结构：
 * - symbol_info: 单个符号的完整描述（名称、地址、大小）
 * - elf_symbol_collection: 符号集合管理（红黑树根节点）
 * 
 * 查找算法：
 * - 红黑树实现，按符号起始地址排序
 * - 支持地址范围查询，用于运行时符号化
 * - 时间复杂度：O(log n)，n为符号数量
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "../include/header.h"

/**
 * @brief 打印ELF文件的完整符号表（调试工具）
 * @param fd ELF文件描述符
 * @param shdrs ELF节区头表
 * @param shnum 节区数量
 * @param shstrtab 节区字符串表
 * 
 * 功能说明：
 * - 遍历所有节区，查找符号表(.symtab)
 * - 提取并打印每个符号的详细信息
 * - 用于调试ELF文件结构和符号信息
 * 
 * 输出格式：
 * Symbol: 符号名称
 *   Value: 符号值（地址）
 *   Size: 符号大小
 *   Info: 符号类型和绑定信息
 *   Other: 符号可见性
 *   Section index: 所属节区索引
 */
void print_symbol_table(int fd, Elf64_Shdr *shdrs, uint16_t shnum, char *shstrtab) {
    Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
    if (elf == NULL) {
        fprintf(stderr, "Error: elf_begin() failed: %s\n", elf_errmsg(-1));
        return;
    }

    for (int i = 0; i < shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            GElf_Shdr sym_shdr;
            gelf_getshdr(elf_getscn(elf, i), &sym_shdr);
            Elf_Data *data = elf_getdata(elf_getscn(elf, i), NULL);

            int num_symbols = sym_shdr.sh_size / sym_shdr.sh_entsize;
            for (int j = 0; j < num_symbols; j++) {
                GElf_Sym sym;
                gelf_getsym(data, j, &sym);

                const char *name = elf_strptr(elf, sym_shdr.sh_link, sym.st_name);
                printf("Symbol: %s\n", name);
                printf("  Value: 0x%lx\n", sym.st_value);
                printf("  Size: %lu\n", sym.st_size);
                printf("  Info: %u\n", sym.st_info);
                printf("  Other: %u\n", sym.st_other);
                printf("  Section index: %u\n", sym.st_shndx);
            }
        }
    }

    elf_end(elf);
}

/**
 * @brief 根据地址查找具体的ELF符号（调试工具）
 * @param fd ELF文件描述符
 * @param shdrs ELF节区头表
 * @param shnum 节区数量
 * @param shstrtab 节区字符串表
 * @param address 要查找的相对地址
 * 
 * 查找逻辑：
 * - 遍历符号表中的所有符号
 * - 检查地址是否落在符号范围内
 * - 找到匹配符号时立即返回详细信息
 * 
 * 地址范围判断：
 * address >= symbol_start && address < (symbol_start + symbol_size)
 */
void find_symbol_by_address(int fd, Elf64_Shdr *shdrs, uint16_t shnum, char *shstrtab, uint64_t address) {
    Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
    if (elf == NULL) {
        fprintf(stderr, "Error: elf_begin() failed: %s\n", elf_errmsg(-1));
        return;
    }

    for (int i = 0; i < shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            GElf_Shdr sym_shdr;
            gelf_getshdr(elf_getscn(elf, i), &sym_shdr);
            Elf_Data *data = elf_getdata(elf_getscn(elf, i), NULL);

            int num_symbols = sym_shdr.sh_size / sym_shdr.sh_entsize;
            for (int j = 0; j < num_symbols; j++) {
                GElf_Sym sym;
                gelf_getsym(data, j, &sym);

                // 地址范围匹配：检查地址是否在符号范围内
                if (address >= sym.st_value && address < (sym.st_value + sym.st_size)) {
                    const char *name = elf_strptr(elf, sym_shdr.sh_link, sym.st_name);
                    printf("Found symbol for address 0x%lx:\n", address);
                    printf("  Symbol: %s\n", name);
                    printf("  Value: 0x%lx\n", sym.st_value);
                    printf("  Size: %lu\n", sym.st_size);
                    printf("  Info: %u\n", sym.st_info);
                    printf("  Other: %u\n", sym.st_other);
                    printf("  Section index: %u\n", sym.st_shndx);
                    elf_end(elf);
                    return;
                }
            }
        }
    }

    printf("No symbol found for address 0x%lx\n", address);
    elf_end(elf);
}

/**
 * @brief 在符号红黑树中查找指定地址的符号
 * @param root 符号红黑树的根节点
 * @param addr 要查找的相对地址
 * @return struct symbol_info* 找到的符号信息，未找到返回NULL
 * 
 * 查找算法：
 * 1. 从根节点开始遍历红黑树
 * 2. 比较地址与符号的起始地址和结束地址
 * 3. 根据比较结果选择左子树或右子树继续查找
 * 4. 找到匹配符号时返回符号信息
 * 
 * 复杂度：O(log n)，n为符号数量
 * 典型场景：ELF文件中通常有1000-10000个符号
 */
struct symbol_info* rb_search_symbol(struct rb_root *root, uint64_t addr) {
    struct rb_node *node = root->rb_node;

    while (node) {
        struct symbol_info *sym = rb_entry(node, struct symbol_info, symbol_rb_node);

        // 地址范围检查：地址是否在符号定义的范围内
        if (addr >= sym->symbol_start && addr < sym->symbol_start + sym->symbol_size) {
            return sym;
        }

        // 二叉查找：根据地址大小选择子树
        if (addr < sym->symbol_start)
            node = node->rb_left;  // 地址在符号之前，查找左子树
        else
            node = node->rb_right; // 地址在符号之后，查找右子树
    }
    return NULL;  // 未找到匹配符号
}

/**
 * @brief 根据相对地址查找ELF文件中的符号名称
 * @param elf ELF文件结构指针
 * @param relative_address 相对于ELF文件的地址偏移
 * @return const char* 符号名称字符串，失败返回错误标识
 * 
 * 返回值说明：
 * - 成功：返回符号的完整名称（如"malloc"）
 * - 失败：
 *   - "unknown_elf_or_no_symbols": ELF文件无效或无符号
 *   - "unknown_function": 地址无对应符号
 * 
 * 使用场景：
 * - 将perf采样的运行时地址转换为可读函数名
 * - 支持共享库和可执行文件的符号化
 * - 处理位置无关代码（PIC）的地址重定位
 * 
 * 线程安全：
 * - 只读访问，支持并发读取
 * - 返回的符号名称字符串为常量，无需释放
 */
const char* find_symbol_name_from_elf(struct elf_file* elf, uint64_t relative_address) {
    // 参数验证：确保ELF文件和符号集合有效
    if (!elf || !elf->symbols || elf->symbols->total_symbols == 0) {
        return "unknown_elf_or_no_symbols";
    }

    // 在符号红黑树中查找匹配地址的符号
    const struct symbol_info *sym = rb_search_symbol(&elf->symbols->symbol_tree, relative_address);
    if (sym != NULL) {
        return sym->symbol_name;  // 返回找到的有效符号名称
    }
    
    return "unknown_function";  // 地址无对应符号
}
