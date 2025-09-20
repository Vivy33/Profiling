#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel_symbol.h"

// 定义 kallsyms 文件路径
#define KALLSYMS_PATH "/proc/kallsyms"

/**
 * @brief 将一个新的内核符号插入到红黑树中。
 *
 * @param root 红黑树的根节点。
 * @param new_symbol 需要插入的新符号。
 * @return 成功返回 true，失败返回 false。
 */
static bool insert_symbol(struct rb_root *root, struct kernel_symbol *new_symbol) {
    struct rb_node **link = &root->rb_node;
    struct rb_node *parent = NULL;

    // 查找插入位置
    while (*link) {
        parent = *link;
        struct kernel_symbol *entry = rb_entry(parent, struct kernel_symbol, node);
        if (new_symbol->address < entry->address) {
            link = &(*link)->rb_left;
        } else if (new_symbol->address > entry->address) {
            link = &(*link)->rb_right;
        } else {
            // 地址冲突，正常情况下 kallsyms 不应有重复地址
            // 如果存在，我们选择保留第一个遇到的符号
            return false;
        }
    }

    // 插入新节点并重新平衡红黑树
    rb_link_node(&new_symbol->node, parent, link);
    rb_insert_color(&new_symbol->node, root);
    return true;
}

/**
 * @brief 从 /proc/kallsyms 文件加载内核符号表。
 */
struct rb_root *load_kernel_symbols(void) {
    FILE *file = fopen(KALLSYMS_PATH, "r");
    if (!file) {
        perror("错误：无法打开 /proc/kallsyms");
        return NULL;
    }

    // 初始化一个空的红黑树
    struct rb_root *root = (struct rb_root *)malloc(sizeof(struct rb_root));
    if (!root) {
        fclose(file);
        return NULL;
    }
    *root = RB_ROOT;

    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    // 逐行读取文件
    /*
    man getline
    DESCRIPTION
       getline()  reads  an entire line from stream, storing the address of the buffer containing the text into *lineptr.  The buffer is null-terminated and includes
       the newline character, if one was found.

       If *lineptr is set to NULL before the call, then getline() will allocate a buffer for storing the line.  This buffer should be freed by the user program  even
       if getline() failed.

       Alternatively,  before  calling getline(), *lineptr can contain a pointer to a malloc(3)-allocated buffer *n bytes in size.  If the buffer is not large enough
       to hold the line, getline() resizes it with realloc(3), updating *lineptr and *n as necessary.

       In either case, on a successful call, *lineptr and *n will be updated to reflect the buffer address and allocated size respectively.

       getdelim() works like getline(), except that a line delimiter other than newline can be specified as the delimiter argument.  As with getline(),  a  delimiter
       character is not added if one was not present in the input before end of file was reached.
    */
    while ((read = getline(&line, &len, file)) != -1) {
        unsigned long address;
        char type;
        char name[256];

        // 解析行: <address> <type> <name>
        if (sscanf(line, "%lx %c %255s", &address, &type, name) == 3) {
            // 只插入全局文本符号 (t, T)
            if (type == 't' || type == 'T') {
                struct kernel_symbol *sym = (struct kernel_symbol *)malloc(sizeof(struct kernel_symbol));
                
                if (!sym) continue;
                sym->name = strdup(name); // 为符号名分配足够的空间

                if (!sym->name) {
                    free(sym);
                    continue;
                }
                sym->address = address;
                sym->type = type;

                if (!insert_symbol(root, sym)) {
                    // printf("插入%s符号失败,", sym->name); // 注释掉，因为重复地址是正常现象
                    free(sym->name);
                    free(sym);
                }
            }
        }
    }

    free(line);
    fclose(file);
    return root;
}

/**
 * @brief 根据给定地址查找对应的内核函数名。
 */
const char *find_kernel_symbol(struct rb_root *root, uint64_t addr) {
    if (!root || !root->rb_node) {
        return "unknown_kernel_function";
    }

    struct rb_node *node = root->rb_node;
    struct kernel_symbol *found_symbol = NULL;

    // 在红黑树中查找最佳匹配的符号
    while (node) {
        struct kernel_symbol *current_symbol = rb_entry(node, struct kernel_symbol, node);
        if (addr >= current_symbol->address) {
            // 如果当前符号地址小于等于目标地址，它是一个潜在的匹配项
            // 我们记录下来，并尝试在右子树中寻找更接近的地址
            found_symbol = current_symbol;
            node = node->rb_right;
        } else {
            // 如果当前符号地址大于目标地址，则匹配项必须在左子树
            node = node->rb_left;
        }
    }

    if (found_symbol) {
        return found_symbol->name;
    }

    return "unknown_kernel_function";
}

/**
 * @brief 释放内核符号表占用的所有资源。
 */
void free_kernel_symbols(struct rb_root *root) {
    if (!root) return;

    struct rb_node *node = rb_first(root);
    while (node) {
        struct kernel_symbol *sym = rb_entry(node, struct kernel_symbol, node);
        struct rb_node *next = rb_next(node);
        rb_erase(node, root);
        free(sym->name);
        free(sym);
        node = next;
    }
    free(root);
}