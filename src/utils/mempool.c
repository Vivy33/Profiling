#include <stdlib.h>
#include "include/mempool.h"

/**
 * @brief 创建并初始化一个新的内存池。
 *
 * 该函数负责分配内存池本身（`struct mempool_s`）、存储内存块指针的数组（`pool`），
 * 以及预分配所有内存块（chunk）。它会一次性将所有需要的内存都申请好，为后续的快速分配做准备。
 *
 * @param pool_size 内存池的容量，即希望预分配的内存块数量。
 * @param chunk_size 每个内存块的大小（字节）。
 * @return 成功时返回指向新创建的内存池的指针，若任何内存分配失败则返回NULL。
 */
struct mempool_s *mempool_create(size_t pool_size, size_t chunk_size) {
    // 1. 为内存池结构体本身分配内存
    struct mempool_s *mp = (struct mempool_s *)malloc(sizeof(struct mempool_s));
    if (!mp) return NULL;

    // 2. 为存储内存块指针的数组分配内存
    mp->pool = (void **)malloc(pool_size * sizeof(void *));
    if (!mp->pool) {
        free(mp);
        return NULL;
    }

    // 3. 初始化内存池的元数据
    mp->pool_size = pool_size;
    mp->chunk_size = chunk_size;
    mp->count = pool_size; // 初始时，所有内存块都可用

    // 4. 预分配所有的内存块，并将它们的指针存入数组
    for (size_t i = 0; i < pool_size; ++i) {
        mp->pool[i] = malloc(chunk_size);
        if (!mp->pool[i]) {
            // 如果中途某个内存块分配失败，需要回滚，释放所有已分配的资源
            for (size_t j = 0; j < i; ++j) {
                free(mp->pool[j]);
            }
            free(mp->pool);
            free(mp);
            return NULL;
        }
    }

    // 5. 初始化互斥锁，用于后续操作的线程安全
    pthread_mutex_init(&mp->lock, NULL);
    return mp;
}

/**
 * @brief 销毁一个内存池并释放所有相关资源。
 *
 * 该函数会释放内存池中所有预分配的内存块，然后释放存储指针的数组，
 * 最后销毁互斥锁并释放内存池结构体本身。
 *
 * @param mp 需要销毁的内存池的指针。
 */
void mempool_destroy(struct mempool_s *mp) {
    if (!mp) return;

    // 1. 释放所有预分配的内存块
    // 注意：这里假设所有在创建时分配的块都由内存池生命周期管理。
    // 如果有块被分配出去但未归还，这会导致这些块的内存被释放。
    for (size_t i = 0; i < mp->pool_size; ++i) {
        free(mp->pool[i]);
    }

    // 2. 释放存储指针的数组
    free(mp->pool);
    // 3. 销毁互斥锁
    pthread_mutex_destroy(&mp->lock);
    // 4. 释放内存池结构体
    free(mp);
}

/**
 * @brief 从内存池中分配一个内存块（线程安全）。
 *
 * 该函数首先锁定互斥锁，然后检查池中是否还有可用的内存块。如果有，
 * 它会像从栈顶弹出一个元素一样，返回最后一个可用的内存块，并减少计数。
 * 如果池已空，它会回退到 malloc，以避免丢样本。
 *
 * @param mp 内存池的指针。
 * @return 成功时返回一个指向内存块的指针，如果池已空则返回NULL。
 */
void *mempool_alloc(struct mempool_s *mp) {
    if (!mp) return NULL;

    pthread_mutex_lock(&mp->lock);
    if (mp->count == 0) {
        // 内存池耗尽时，回退到动态分配，避免丢样本。
        pthread_mutex_unlock(&mp->lock);
        return malloc(mp->chunk_size);
    }

    // 从“栈顶”取出一个内存块指针
    void *chunk = mp->pool[--mp->count];
    pthread_mutex_unlock(&mp->lock);

    return chunk;
}

/**
 * @brief 将一个内存块归还到内存池中（线程安全）。
 *
 * 该函数首先锁定互斥锁，然后检查内存池是否已满。如果未满，
 * 它会将归还的内存块指针存放到“栈顶”，并增加计数。如果池已满，
 * 它会直接`free`掉这个内存块，以防止内存池无限增长或内存泄漏。
 * （注意：当前实现是如果池满则什么都不做，这依赖于`mempool_alloc`在池空时返回NULL
 *  且调用方会`malloc`，而`free`时则直接`free`，而不是调用`mempool_free`）
 *  为了安全，如果池满，应该直接释放chunk。
 *
 * @param mp 内存池的指针。
 * @param chunk 需要归还的内存块的指针。
 */
void mempool_free(struct mempool_s *mp, void *chunk) {
    if (!mp || !chunk) return;

    pthread_mutex_lock(&mp->lock);
    if (mp->count < mp->pool_size) {
        // 如果内存池未满，将内存块放回池中
        mp->pool[mp->count++] = chunk;
        pthread_mutex_unlock(&mp->lock);
    } else {
        // 如果内存池已满，直接释放这个chunk，防止内存泄漏。
        // 这种情况通常发生在：内存池耗尽后，调用者通过malloc分配了新的内存块，
        // 使用完毕后，依然尝试通过mempool_free归还。
        pthread_mutex_unlock(&mp->lock);
        free(chunk);
    }
}