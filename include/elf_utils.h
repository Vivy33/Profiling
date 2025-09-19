#ifndef ELF_UTILS_H
#define ELF_UTILS_H

#include "header.h"

// 查找或创建ELF文件对象
struct elf_file* find_or_create_elf(struct system_context* sys, int pid, const char *filename);

// 减少ELF文件的引用计数。如果引用计数归零，则移除该文件。
void release_elf(struct elf_file_cache* elf_table, const char* filename);

// 清空整个ELF缓存，释放所有相关内存
void clear_elf_cache(struct elf_file_cache* elf_table);

//用于将一个数值 n 对齐到4字节边界。
//对齐到4字节边界是为了确保数据在内存中的存放符合某些硬件平台的对齐要求，从而提高内存访问的效率和正确性。
#define NOTE_ALIGN(n) (((n) + 3) & -4U)

#endif // ELF_UTILS_H