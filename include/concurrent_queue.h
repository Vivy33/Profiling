#ifndef CONCURRENT_QUEUE_H
#define CONCURRENT_QUEUE_H

#include <pthread.h>
#include <stdbool.h>

/**
 * @file concurrent_queue.h
 * @brief 一个线程安全的并发队列接口。
 *
 * 定义了并发队列所需的数据结构和操作函数。
 * 这是一个基于有界缓冲区、使用互斥锁和条件变量实现的经典生产者-消费者队列。
 */

/**
 * @brief 并发队列的上下文结构体（不透明）。
 *
 * 封装了队列的所有内部状态，包括缓冲区、容量、大小、头尾指针、
 * 互斥锁、条件变量以及关闭标志。
 */
typedef struct concurrent_queue_t concurrent_queue_t;

/**
 * @brief 初始化一个新的并发队列。
 *
 * @param capacity 队列的最大容量。
 * @return 成功时返回队列的指针，失败时返回 NULL。
 */
concurrent_queue_t* queue_init(int capacity);

/**
 * @brief 销毁一个并发队列。
 *
 * 释放队列本身及其内部缓冲区所占用的内存，并销毁相关的锁和条件变量。
 * 注意：此函数不会释放队列中存储的元素所指向的内存。
 *
 * @param queue 要销毁的队列的指针。
 */
void queue_destroy(concurrent_queue_t* queue);
int queue_get_size(concurrent_queue_t* queue);

/**
 * @brief 向队列中推送一个元素（生产者）。
 *
 * 如果队列已满，此函数会阻塞，直到队列中有空间可用或队列被关闭。
 *
 * @param queue 队列的指针。
 * @param item 要添加到队列中的元素的指针。
 */
void queue_push(concurrent_queue_t* queue, void* item);

/**
 * @brief 从队列中弹出一个元素（消费者）。
 *
 * 如果队列为空，此函数会阻塞，直到队列中有元素可用或队列被关闭。
 *
 * @param queue 队列的指针。
 * @return 返回队列头部的元素指针。如果队列被关闭且为空，则返回 NULL。
 */
void* queue_pop(concurrent_queue_t* queue);

/**
 * @brief 从队列中批量弹出一组元素（消费者）。
 *
 * 如果队列为空，此函数会阻塞，直到队列中有元素可用或队列被关闭。
 *
 * @param queue 队列的指针。
 * @param items 用于存储弹出元素的数组。
 * @param max_items 要弹出的最大元素数量。
 * @return 实际弹出的元素数量。如果队列被关闭且为空，则返回 0。
 */
int queue_pop_batch(concurrent_queue_t* queue, void** items, int max_items);

/**
 * @brief 向队列发送关闭信号。
 *
 * 通知所有正在等待的生产者和消费者线程，队列即将关闭。
 * 这会唤醒所有阻塞在 push 或 pop 操作上的线程。
 *
 * @param queue 队列的指针。
 */
void queue_signal_shutdown(concurrent_queue_t* queue);



#endif // CONCURRENT_QUEUE_H