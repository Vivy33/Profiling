#include "../include/hash.h"
#include "../include/header.h"

// 哈希函数 - 用于PID
unsigned int hash_pid(int pid) {
    return pid % HASHTABLE_SIZE;
}

// 哈希函数 - 用于字符串
unsigned int hash_str(const char *str) {
    unsigned int hash = 0;
    while (*str) {
        hash = (hash << 5) - hash + *str++;
    }
    return hash % HASHTABLE_SIZE;
}