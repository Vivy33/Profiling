#include <stdlib.h>
#include <pthread.h>
#include "include/concurrent_queue.h"

/**
 * @file concurrent_queue.c
 * @brief 线程安全并发队列的实现。
 *
 * 使用 POSIX 线程（pthread）的互斥锁和条件变量，
 * 实现了一个有界的、线程安全的生产者-消费者队列。
 */

/**
 * @brief 并发队列的上下文结构体。
 */
struct concurrent_queue_t {
    void** buffer;              // 用于存储元素的循环缓冲区
    int capacity;               // 缓冲区容量
    int size;                   // 当前队列中的元素数量
    int head;                   // 队头指针（下一个要 pop 的位置）
    int tail;                   // 队尾指针（下一个要 push 的位置）
    pthread_mutex_t mutex;      // 互斥锁，保护对队列的访问
    pthread_cond_t cond_full;   // 条件变量，当队列满时，生产者在此等待
    pthread_cond_t cond_empty;  // 条件变量，当队列空时，消费者在此等待
    volatile bool shutdown;     // 关闭标志
};

/**
 * @brief 初始化一个新的并发队列。
 */
concurrent_queue_t* queue_init(int capacity) {
    concurrent_queue_t* queue = (concurrent_queue_t*)malloc(sizeof(concurrent_queue_t));
    if (!queue) return NULL;

    queue->buffer = (void**)malloc(sizeof(void*) * capacity);
    if (!queue->buffer) {
        free(queue);
        return NULL;
    }

    queue->capacity = capacity;
    queue->size = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->shutdown = false;

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond_full, NULL);
    pthread_cond_init(&queue->cond_empty, NULL);

    return queue;
}

/**
 * @brief 销毁一个并发队列。
 */
void queue_destroy(concurrent_queue_t* queue) {
    if (!queue) return;
    // 注意：这里不释放队列中元素指向的内存
    free(queue->buffer);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond_full);
    pthread_cond_destroy(&queue->cond_empty);
    free(queue);
}

/**
 * @brief 向队列中推送一个元素（生产者）。
 */
void queue_push(concurrent_queue_t* queue, void* item) {
    pthread_mutex_lock(&queue->mutex);

    // 当队列满且未关闭时，等待
    while (queue->size == queue->capacity && !queue->shutdown) {
        pthread_cond_wait(&queue->cond_full, &queue->mutex);
    }

    // 如果队列已关闭，则直接返回
    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return;
    }

    // 将元素放入队尾
    queue->buffer[queue->tail] = item;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->size++;

    // 通知可能正在等待的消费者
    pthread_cond_signal(&queue->cond_empty);
    pthread_mutex_unlock(&queue->mutex);
}

/**
 * @brief 从队列中弹出一个元素（消费者）。
 */
void* queue_pop(concurrent_queue_t* queue) {
    pthread_mutex_lock(&queue->mutex);

    // 当队列为空且未关闭时，等待
    while (queue->size == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->cond_empty, &queue->mutex);
    }

    // 如果队列已关闭且为空，则返回 NULL，表示结束
    if (queue->shutdown && queue->size == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    // 从队头取出一个元素
    void* item = queue->buffer[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;

    // 通知可能正在等待的生产者
    pthread_cond_signal(&queue->cond_full);
    pthread_mutex_unlock(&queue->mutex);

    return item;
}

/**
 * @brief 从队列中批量弹出一组元素（消费者）。
 */
int queue_pop_batch(concurrent_queue_t* queue, void** items, int max_items, long long *latency_ns) {
    pthread_mutex_lock(&queue->mutex);

    // 当队列为空且未关闭时，等待
    while (queue->size == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->cond_empty, &queue->mutex);
    }

    // 如果队列已关闭且为空，则返回 0，表示结束
    if (queue->shutdown && queue->size == 0) {
        pthread_mutex_unlock(&queue->mutex);
        if (latency_ns) *latency_ns = 0;
        return 0;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int actual_pop_count = 0;
    // 实际要弹出的数量是 max_items 和当前队列大小的最小值
    int count_to_pop = (queue->size < max_items) ? queue->size : max_items;

    for (int i = 0; i < count_to_pop; ++i) {
        items[i] = queue->buffer[queue->head];
        queue->head = (queue->head + 1) % queue->capacity;
        queue->size--;
        actual_pop_count++;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    if (latency_ns) {
        *latency_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
    }

    // 通知可能正在等待的生产者
    if (actual_pop_count > 0) {
        pthread_cond_signal(&queue->cond_full);
    }
    pthread_mutex_unlock(&queue->mutex);

    return actual_pop_count;
}

/**
 * @brief 向队列中批量推送一组元素（生产者）。
 */
void queue_push_batch(concurrent_queue_t* queue, void** items, int count, long long *latency_ns) {
    if (!queue || !items || count <= 0) {
        if (latency_ns) *latency_ns = 0;
        return;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    pthread_mutex_lock(&queue->mutex);

    // 当队列剩余空间不足以容纳整个批次，且未关闭时，等待
    while ((queue->capacity - queue->size) < count && !queue->shutdown) {
        pthread_cond_wait(&queue->cond_full, &queue->mutex);
    }

    // 如果队列已关闭，则直接返回
    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        if (latency_ns) {
            clock_gettime(CLOCK_MONOTONIC, &end);
            *latency_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
        }
        return;
    }

    // 批量将元素放入队尾
    for (int i = 0; i < count; ++i) {
        queue->buffer[queue->tail] = items[i];
        queue->tail = (queue->tail + 1) % queue->capacity;
    }
    queue->size += count;

    // 通知可能正在等待的消费者
    // 使用 broadcast 而不是 signal，以防有多个消费者在等待
    pthread_cond_broadcast(&queue->cond_empty);
    pthread_mutex_unlock(&queue->mutex);

    if (latency_ns) {
        clock_gettime(CLOCK_MONOTONIC, &end);
        *latency_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
    }
}


/**
 * @brief 向队列发送关闭信号。
 */
void queue_signal_shutdown(concurrent_queue_t* queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = true;
    // 唤醒所有可能在等待的线程
    pthread_cond_broadcast(&queue->cond_empty);
    pthread_cond_broadcast(&queue->cond_full);
    pthread_mutex_unlock(&queue->mutex);
}

/**
 * @brief 获取队列当前的大小。
 */
int queue_get_size(concurrent_queue_t* queue) {
    if (!queue) return 0;

    /**
     * 这里加锁的本意是防止读到的size是个过时或不正确的值
     * 但加锁也会有可能过时，考虑下面的时序
     *  size
     *        push
     *  pop
     * push和pop都已经加过锁了，这个时序下，size加锁没有意义
     * size加锁是对的，但是这个精度没有必要，加锁防止的是正确性问题，同时+1 -1
     * size是只读的，它不会单独操作，它后面的write一定是加锁自己判断size的
     * 数据过时，和加锁无关，加锁只是解决了push/pop size
     * 但是锁解决不了超过锁范围的ordering问题
     * 从做监控的角度来看，取size延迟了1ms，影响不大
     * 我们看的是一个整体趋势，不在乎绝对精度，或者说，我们看的是某个时间点附近的数据
     * 加锁是一定对的，而且目前测试是没有性能瓶颈问题，所以暂时保留这把锁
     */
    pthread_mutex_lock(&queue->mutex);
    int size = queue->size;
    pthread_mutex_unlock(&queue->mutex);
    return size;
}