#ifndef ELF_UTILS_H
#define ELF_UTILS_H

#include "header.h"

//用于将一个数值 n 对齐到4字节边界。
//对齐到4字节边界是为了确保数据在内存中的存放符合某些硬件平台的对齐要求，从而提高内存访问的效率和正确性。
#define NOTE_ALIGN(n) (((n) + 3) & -4U)

#endif // ELF_UTILS_H