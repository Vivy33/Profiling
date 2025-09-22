#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "../../include/concurrent_queue.h"

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
int queue_size(concurrent_queue_t* queue) {
    pthread_mutex_lock(&queue->mutex);
    int size = queue->size;
    pthread_mutex_unlock(&queue->mutex);
    return size;
}