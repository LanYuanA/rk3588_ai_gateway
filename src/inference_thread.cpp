#include "inference_thread.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "app_context.h"
#include "rknn_detector.h"

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
    if (!detector.init(model_path, npu_core_index)) {
        std::cerr << "[错误] NPU " << npu_thread_id << " 模型初始化失败。" << std::endl;
        return;
    }

    int idx = 0;
    while (g_system_running) {
        int current_stream_id = handled_streams[idx];
        VideoFrame frame;

        if (g_inference_queues[current_stream_id].size() > 0) {
            while (g_inference_queues[current_stream_id].size() > 1) {
                VideoFrame dummy;
                g_inference_queues[current_stream_id].pop(dummy);
            }

            if (g_inference_queues[current_stream_id].pop(frame)) {
                std::vector<DetectResult> results = detector.inference(frame.image);

                {
                    std::lock_guard<std::mutex> lock(g_results_mutex[current_stream_id]);
                    g_latest_results[current_stream_id] = results;
                }
            }
        }

        idx = (idx + 1) % handled_streams.size();
        if (idx == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::cout << "[推理线程 NPU " << npu_thread_id << "] 退出." << std::endl;
}
