#include "inference_thread.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "app_context.h"
#include "rknn_detector.h"

// 保留原有的inferenceThread函数，但标记为废弃
// 新的实现使用npu_pool.cpp中的npuWorkerThread和inferenceDispatcherThread
void inferenceThread(const std::string& model_path,
                     std::vector<int> handled_streams,
                     int npu_thread_id,
                     int npu_core_index) {
    if (handled_streams.empty()) {
        return;
    }

    std::cout << "[推理线程 NPU " << npu_thread_id << "] 启动，接管流: ";
    for (int s : handled_streams) {
        std::cout << s << " ";
    }
    std::cout << "准备加载模型..." << std::endl;

    RKNNDetector detector;
    //从model_path加载模型，并且给当前推理线程分配npu核
    if (!detector.init(model_path, npu_core_index)) {
        std::cerr << "[错误] NPU " << npu_thread_id << " 模型初始化失败。" << std::endl;
        return;
    }

    int idx = 0;
    int empty_cycles = 0;
    while (g_system_running) {
        int current_stream_id = handled_streams[idx];
        VideoFrame frame;

        // 跳过空队列，避免无意义的轮询
        if (g_inference_queues[current_stream_id].size() == 0) {
            idx = (idx + 1) % handled_streams.size();
            if (idx == 0) {
                empty_cycles++;
                if (empty_cycles > 100) {
                    // 所有队列持续为空，长睡避免空转
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    empty_cycles = 0;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            continue;
        }
        empty_cycles = 0;

        // 丢弃旧帧，只保留最新
        int dropped = 0;
        while (g_inference_queues[current_stream_id].size() > 1) {
            VideoFrame dummy;
            g_inference_queues[current_stream_id].pop(dummy);
            dropped++;
        }
        if (dropped > 0 && dropped % 30 == 0) {
            std::cerr << "[推理丢帧] 流 " << current_stream_id
                      << " 丢弃 " << dropped << " 帧" << std::endl;
        }

        if (g_inference_queues[current_stream_id].pop(frame)) {
            std::vector<DetectResult> results = detector.inference(*frame.image);
            {
                std::lock_guard<std::mutex> lock(g_results_mutex[current_stream_id]);
                g_latest_results[current_stream_id] = results;
            }
        }

        idx = (idx + 1) % handled_streams.size();
        if (idx == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::cout << "[推理线程 NPU " << npu_thread_id << "] 退出." << std::endl;
}
