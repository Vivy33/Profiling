#include "memory_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// 全局内存管理器
struct memory_manager global_memory_manager = {0};

// 初始化内存管理器
int memory_manager_init(void) {
    if (global_memory_manager.initialized) {
        return 0;
    }

    memset(&global_memory_manager, 0, sizeof(global_memory_manager));

    // 初始化各个子系统
    cache_init(-1, 0);  // 初始化缓存系统
    vma_cache_init(MM_CONFIG_DEFAULT_VMA_CACHE_SIZE);
    symbol_cache_init(MM_CONFIG_DEFAULT_SYMBOL_CACHE_SIZE);

    global_memory_manager.initialized = 1;

    return 0;
}

// 清理内存管理器
void memory_manager_cleanup(void) {
    if (!global_memory_manager.initialized) {
        return;
    }

    

    // 清理各个子系统
    cache_cleanup(global_memory_manager.file_cache);
    vma_cache_cleanup();
    symbol_cache_cleanup();

    

    memset(&global_memory_manager, 0, sizeof(global_memory_manager));
}

// 内存映射文件（带缓存优化）
void* memory_manager_mmap_file(const char* filename, size_t* file_size) {
    if (!filename || !file_size) return MAP_FAILED;

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        return MAP_FAILED;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return MAP_FAILED;
    }

    *file_size = st.st_size;

    // 使用MAP_SHARED按需映射，替代MAP_POPULATE
    void* addr = mmap(NULL, *file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        return MAP_FAILED;
    }

    close(fd);
    return addr;
}

// 取消内存映射
int memory_manager_munmap_file(void* addr, size_t length) {
    if (!addr) return -1;
    return munmap(addr, length);
}

// 从文件读取数据（带缓存）
int memory_manager_read_file(const char* filename, uint64_t offset, void* buffer, size_t size) {
    if (!filename || !buffer) return -1;

    


    // 这里应该使用文件缓存，简化实现
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
            return -1;
    }

    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        close(fd);
            return -1;
    }

    ssize_t bytes_read = read(fd, buffer, size);
    close(fd);

    if (bytes_read > 0) {
        global_memory_manager.stats.total_accesses++;
        global_memory_manager.stats.cache_misses++;
    }

    return (bytes_read == (ssize_t)size) ? 0 : -1;
}

// 查找VMA信息（带缓存）
struct vma_cache_entry* memory_manager_find_vma(uint64_t addr) {
    if (!global_memory_manager.initialized) {
        memory_manager_init();
    }

    


    struct vma_cache_entry* vma = vma_cache_lookup(addr);
    if (vma) {
        global_memory_manager.stats.cache_hits++;
    } else {
        global_memory_manager.stats.cache_misses++;
    }

    return vma;
}

// 查找符号信息（带缓存）
const char* memory_manager_find_symbol(uint64_t addr, const char* filename) {
    if (!global_memory_manager.initialized) {
        memory_manager_init();
    }

    

    const char* symbol = symbol_cache_get_name(addr, filename);
    if (symbol) {
        global_memory_manager.stats.cache_hits++;
    } else {
        global_memory_manager.stats.cache_misses++;
    }

    return symbol;
}

// 获取相对地址（带缓存优化）
uint64_t memory_manager_get_relative_addr(uint64_t real_addr, const char* filename) {
    if (!global_memory_manager.initialized) {
        memory_manager_init();
    }

    

    uint64_t relative_addr = vma_cache_get_relative_addr(real_addr, filename);

    return relative_addr;
}


// 获取内存管理统计信息
void memory_manager_get_stats(uint64_t* hits, uint64_t* misses, uint64_t* evictions) {
    

    *hits = global_memory_manager.stats.cache_hits;
    *misses = global_memory_manager.stats.cache_misses;
    *evictions = global_memory_manager.stats.cache_evictions;

}

// 强制刷新所有缓存
void memory_manager_flush_all(void) {
    

    cache_flush(NULL);  // 刷新文件缓存
    vma_cache_flush();  // 刷新VMA缓存
    symbol_cache_flush(); // 刷新符号缓存

    // 重置统计信息
    global_memory_manager.stats.cache_hits = 0;
    global_memory_manager.stats.cache_misses = 0;
    global_memory_manager.stats.cache_evictions = 0;

}

// 内存使用信息
void memory_manager_print_usage(void) {
    

    printf("=== Memory Manager Usage ===\n");
    printf("Total Accesses: %lu\n", global_memory_manager.stats.total_accesses);
    printf("Cache Hits: %lu\n", global_memory_manager.stats.cache_hits);
    printf("Cache Misses: %lu\n", global_memory_manager.stats.cache_misses);
    printf("Cache Evictions: %lu\n", global_memory_manager.stats.cache_evictions);

    double hit_rate = 0.0;
    if (global_memory_manager.stats.total_accesses > 0) {
        hit_rate = (double)global_memory_manager.stats.cache_hits /
                   (global_memory_manager.stats.cache_hits + global_memory_manager.stats.cache_misses) * 100.0;
    }
    printf("Cache Hit Rate: %.2f%%\n", hit_rate);

}

// 初始化全局内存管理器
__attribute__((constructor)) static void init_memory_manager(void) {
    memory_manager_init();
}

// 清理全局内存管理器
__attribute__((destructor)) static void cleanup_memory_manager(void) {
    memory_manager_cleanup();
}