#ifndef KERNEL_SYMBOL_H
#define KERNEL_SYMBOL_H

#include "rbtree.h"
#include <stdint.h>

// 定义内核符号的数据结构
struct kernel_symbol {
    uint64_t address;      // 符号地址
    char type;             // 符号类型 (例如 't', 'T')
    char *name;            // 符号名称
    struct rb_node node;   // 红黑树节点
};

/**
 * @brief 从 /proc/kallsyms 文件加载内核符号表。
 *
 * 该函数读取、解析系统内核符号，并将其组织到一个红黑树中以便快速查找。
 *
 * @return 成功时返回指向内核符号红黑树根节点的指针，
 *         失败时（如文件未找到、内存不足）返回 NULL。
 */
struct rb_root *load_kernel_symbols(void);

/**
 * @brief 根据给定地址查找对应的内核函数名。
 *
 * 函数会搜索小于或等于给定地址的、值最大的那个符号。
 *
 * @param root 内核符号红黑树的根节点。
 * @param addr 需要查询的地址。
 * @return 如果找到，返回一个指向符号名称字符串的指针；否则返回 "unknown_kernel_function"。
 */
const char *find_kernel_symbol(struct rb_root *root, uint64_t addr);

/**
 * @brief 释放内核符号表占用的所有资源。
 *
 * @param root 需要释放的内核符号树的根节点。
 */
void free_kernel_symbols(struct rb_root *root);

#endif // KERNEL_SYMBOL_H