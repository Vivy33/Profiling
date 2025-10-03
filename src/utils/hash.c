#include "include/hash.h"
#include "include/header.h"

// 哈希函数 - 用于PID
unsigned int hash_pid(int pid) {
    // 修复说明：显式将 pid 转换为无符号，避免负数取模导致哈希桶索引为负，
    // 可能引发数组越界访问和崩溃（例如 pid=-1 时）。
    unsigned int upid = (unsigned int)pid;
    return upid % HASHTABLE_SIZE;
}

// 哈希函数 - 用于字符串
unsigned int hash_str(const char *str) {
    unsigned int hash = 0;
    while (*str) {
        hash = (hash << 5) - hash + *str++;
    }
    return hash % HASHTABLE_SIZE;
}