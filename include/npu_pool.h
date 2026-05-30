#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "common.h"
#include "ThreadSafeQueue.h"

// NPU推理任务
struct NpuTask {
    int stream_id;                      // 来源流ID
    uint64_t frame_id;                  // 帧序号
    std::shared_ptr<cv::Mat> image;     // 图像数据（零拷贝）
};

// NPU工作线程统计
struct NpuWorkerStats {
    std::atomic<uint64_t> tasks_processed{0};
    std::atomic<uint64_t> tasks_dropped{0};
    std::atomic<uint64_t> total_latency_us{0};

    void reset() {
        tasks_processed = 0;
        tasks_dropped = 0;
        total_latency_us = 0;
    }
};

// NPU资源池配置
constexpr int NUM_NPU_CORES = 3;  // RK3588有3个NPU核心
constexpr size_t NPU_TASK_QUEUE_SIZE = 10;  // 每个流的队列深度（减小，避免堆积）

// 全局NPU任务队列（每个流独立队列，实现公平调度）
extern ThreadSafeQueue<NpuTask> g_npu_task_queues[4];  // 使用常量值避免循环依赖

// NPU工作线程函数声明
void npuWorkerThread(const std::string& model_path,
                     int npu_core_index,
                     NpuWorkerStats& stats);

// 推理分发线程函数声明
void inferenceDispatcherThread(int stream_id);
