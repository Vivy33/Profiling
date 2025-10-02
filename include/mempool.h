#ifndef MEMPOOL_H
#define MEMPOOL_H

/**
 * @file mempool.h
 * @brief 一个通用的、线程安全的内存池实现。
 *
 * 该内存池用于管理固定大小的内存块（chunk），旨在减少高频率分配和释放
 * 小块内存时`malloc`和`free`带来的开销和内存碎片。它通过预先分配一大块
 * 连续内存，并将其分割成多个chunk进行管理。当需要内存时，直接从池中
 * 取出一个chunk；当内存不再使用时，将其归还到池中，而不是真正释放它。
 * 这种机制特别适用于那些生命周期短、大小固定的对象，例如本项目中的`raw_sample`。
 */

#include <pthread.h>
#include <stddef.h>

/**
 * @brief 内存池结构体定义。
 * 
 * 该结构体定义了一个内存池的核心组成部分。
 */
struct mempool_s {
    void **pool;                /**< 指向内存池存储区的指针数组。它像一个栈，用于存放空闲内存块的地址。 */
    size_t pool_size;           /**< 内存池的总容量，即最多可以容纳多少个内存块。 */
    size_t chunk_size;          /**< 每个内存块（chunk）的大小，单位为字节。 */
    size_t count;               /**< 当前内存池中可用的空闲内存块数量。 */
    pthread_mutex_t lock;       /**< 互斥锁，用于保证在多线程环境下的线程安全。 */
}; 

/**
 * @brief 创建一个新的内存池。
 *
 * @param pool_size 内存池的容量，即预分配的内存块数量。
 * @param chunk_size 每个内存块的大小（字节）。
 * @return 成功时返回指向新创建的内存池的指针，失败时返回NULL。
 */
struct mempool_s *mempool_create(size_t pool_size, size_t chunk_size);

/**
 * @brief 销毁一个内存池并释放所有相关资源。
 *
 * @param mp 需要销毁的内存池的指针。
 */
void mempool_destroy(struct mempool_s *mp);

/**
 * @brief 从内存池中分配一个内存块。
 *
 * 这是一个线程安全的操作。如果池中有可用的内存块，则直接返回一个；
 * 如果没有，则会通过`malloc`动态分配一个新的内存块。这种设计确保了
 * 即使在池耗尽的情况下，程序依然可以正常工作，但性能会有所下降。
 *
 * @param mp 内存池的指针。
 * @return 返回一个指向已分配内存块的指针。
 */
void *mempool_alloc(struct mempool_s *mp);

/**
 * @brief 将一个内存块归还到内存池中。
 *
 * 这是一个线程安全的操作。如果池未满，则将内存块放回池中以备后用；
 * 如果池已满，则直接通过`free`释放该内存块，以避免内存泄漏。
 *
 * @param mp 内存池的指针。
 * @param chunk 需要归还的内存块的指针。
 */
void mempool_free(struct mempool_s *mp, void *chunk);

#endif // MEMPOOL_H