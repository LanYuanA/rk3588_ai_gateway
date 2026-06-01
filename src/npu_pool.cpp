#include "npu_pool.h"

#include <chrono>
#include <iostream>

#include "app_context.h"
#include "realtime_composer.h"
#include "rknn_detector.h"

// 全局NPU任务队列（每个流独立队列）
ThreadSafeQueue<NpuTask> g_npu_task_queues[4] = {
    ThreadSafeQueue<NpuTask>(10),
    ThreadSafeQueue<NpuTask>(10),
    ThreadSafeQueue<NpuTask>(10),
    ThreadSafeQueue<NpuTask>(10)
};

void npuWorkerThread(const std::string& model_path,
                     int npu_core_index,
                     NpuWorkerStats& stats) {
    std::cout << "[NPU工作线程 " << npu_core_index << "] 启动，等待任务..." << std::endl;

    // 初始化NPU检测器
    RKNNDetector detector;
    if (!detector.init(model_path, npu_core_index)) {
        std::cerr << "[错误] NPU核心 " << npu_core_index << " 初始化失败" << std::endl;
        return;
    }

    std::cout << "[NPU工作线程 " << npu_core_index << "] 模型加载成功，开始处理任务" << std::endl;

    NpuTask task;
    int current_stream = 0;  // 轮询起始流
    int empty_cycles = 0;

    while (g_system_running) {
        bool found_task = false;

        // 轮询所有流的队列，实现公平调度
        for (int i = 0; i < 4; ++i) {
            int stream_idx = (current_stream + i) % 4;

            // 尝试从当前流的队列获取任务（非阻塞）
            if (g_npu_task_queues[stream_idx].size() > 0) {
                if (g_npu_task_queues[stream_idx].pop(task)) {
                    found_task = true;
                    current_stream = (stream_idx + 1) % 4;  // 更新轮询位置

                    auto start_time = std::chrono::high_resolution_clock::now();

                    // 执行推理
                    auto results = detector.inference(*task.image);

                    auto end_time = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

                    // 更新统计信息
                    stats.tasks_processed++;
                    stats.total_latency_us += duration.count();

                    // 将结果存入对应流的结果队列
                    {
                        std::lock_guard<std::mutex> lock(g_results_mutex[task.stream_id]);
                        g_latest_results[task.stream_id] = std::move(results);
                    }

                    // 更新实时合成器的检测结果
                    if (g_realtime_composer) {
                        g_realtime_composer->updateDetectionResults(task.stream_id, g_latest_results[task.stream_id]);
                    }

                    break;  // 处理完一个任务后重新开始轮询
                }
            }
        }

        if (!found_task) {
            empty_cycles++;
            if (empty_cycles > 100) {
                // 所有队列都为空，长睡避免空转
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                empty_cycles = 0;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            empty_cycles = 0;
        }
    }

    std::cout << "[NPU工作线程 " << npu_core_index << "] 退出，"
              << "处理任务数: " << stats.tasks_processed
              << ", 平均延迟: " << (stats.tasks_processed > 0 ? stats.total_latency_us / stats.tasks_processed : 0)
              << "μs" << std::endl;
}

void inferenceDispatcherThread(int stream_id) {
    std::cout << "[推理分发线程 流" << stream_id << "] 启动" << std::endl;

    uint64_t frame_id = 0;
    int empty_cycles = 0;

    while (g_system_running) {
        VideoFrame frame;

        // 从流队列获取帧
        if (g_inference_queues[stream_id].size() == 0) {
            empty_cycles++;
            if (empty_cycles > 100) {
                // 长时间无任务，休眠避免空转
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                empty_cycles = 0;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            continue;
        }

        empty_cycles = 0;

        // 丢弃旧帧，只保留最新
        while (g_inference_queues[stream_id].size() > 1) {
            VideoFrame dummy;
            g_inference_queues[stream_id].pop(dummy);
        }

        if (g_inference_queues[stream_id].pop(frame)) {
            // 创建NPU任务
            NpuTask task;
            task.stream_id = stream_id;
            task.frame_id = frame_id++;
            task.image = frame.image;  // 共享指针，零拷贝

            // 提交到对应流的任务队列
            if (!g_npu_task_queues[stream_id].push(std::move(task))) {
                // 队列已满，任务被丢弃
                std::cerr << "[推理分发线程 流" << stream_id << "] 任务队列已满，丢弃帧" << std::endl;
            }
        }
    }

    std::cout << "[推理分发线程 流" << stream_id << "] 退出" << std::endl;
}
