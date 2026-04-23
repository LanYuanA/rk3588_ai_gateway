#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include "common.h"

// 线程安全的缓冲队列，专为实时视频流设计
template <typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    size_t max_size_;

public:
     // 默认最大缓存10帧，平衡内存使用和流畅性，防止处理过慢导致 OOM (内存溢出) 和 极大的视频延迟
     explicit ThreadSafeQueue(size_t max_size = 10) : max_size_(max_size) {}

    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        // 核心逻辑：如果队列满了，丢弃最旧的帧（队头），保证最新帧优先被处理
        if (queue_.size() >= max_size_) {
            queue_.pop(); 
        }
        queue_.push(std::move(value));
        // 通知挂起等待的消费线程
        cond_.notify_one();
    }

    // 阻塞式获取，如果系统退出则返回 false
    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等待条件：队列不为空，或者系统需要退出
        cond_.wait(lock, [this] { return !queue_.empty() || !g_system_running; });
        
        if (!g_system_running && queue_.empty()) {
            return false;
        }
        
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // 立即唤醒所有等待的线程 (用于安全退出)
    void wake_up_all() {
        cond_.notify_all();
    }
    
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
    }
};
