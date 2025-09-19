#ifndef HASH_H
#define HASH_H

#include <stdint.h>

unsigned int hash_pid(int pid);
unsigned int hash_str(const char* str);
uint64_t hash_file(const char* filename);

#endif // HASH_H
