#include "inference_thread.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "app_context.h"
#include "realtime_composer.h"
#include "rknn_detector.h"

// 每路流的最近一次推理结果（用于沿用）
static std::vector<DetectResult> g_last_inference_results[NUM_STREAMS];
static std::mutex g_last_results_mutex[NUM_STREAMS];
static int g_inference_frame_count[NUM_STREAMS] = {0};

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
    while (g_system_running) {
        int current_stream_id = handled_streams[idx];

        // 关键：直接取队列中最新帧，丢弃所有旧帧
        VideoFrame frame;
        bool got_frame = false;

        // 清空队列中所有旧帧，只留最新
        while (g_inference_queues[current_stream_id].size() > 0) {
            VideoFrame latest;
            g_inference_queues[current_stream_id].pop(latest);
            frame = latest;
            got_frame = true;
            // 继续清空，确保取到最新
            if (g_inference_queues[current_stream_id].size() > 0) {
                VideoFrame dummy;
                g_inference_queues[current_stream_id].pop(dummy);
            } else {
                break;
            }
        }

        if (got_frame) {
            // 有新帧，运行推理
            std::vector<DetectResult> results = detector.inference(*frame.image);

            g_inference_frame_count[current_stream_id]++;

            // 每30帧打印一次
            if (g_inference_frame_count[current_stream_id] % 30 == 0) {
                std::cout << "[推理DEBUG] 流" << current_stream_id
                          << " 第" << g_inference_frame_count[current_stream_id] << "次推理"
                          << " 检测到" << results.size() << "个目标";
                for (size_t i = 0; i < results.size() && i < 3; i++) {
                    std::cout << " [conf:" << results[i].confidence
                              << " box:(" << results[i].box.x << "," << results[i].box.y
                              << "," << results[i].box.width << "," << results[i].box.height << ")]";
                }
                std::cout << std::endl;
            }

            // 保存并更新检测结果
            {
                std::lock_guard<std::mutex> lock(g_last_results_mutex[current_stream_id]);
                g_last_inference_results[current_stream_id] = results;
            }
            {
                std::lock_guard<std::mutex> lock(g_results_mutex[current_stream_id]);
                g_latest_results[current_stream_id] = results;
            }
            if (g_realtime_composer) {
                g_realtime_composer->updateDetectionResults(current_stream_id, results);
            }
        } else {
            // 没有新帧，沿用上次的推理结果（保证检测框持续显示）
            std::vector<DetectResult> last_results;
            {
                std::lock_guard<std::mutex> lock(g_last_results_mutex[current_stream_id]);
                last_results = g_last_inference_results[current_stream_id];
            }
            if (!last_results.empty() && g_realtime_composer) {
                g_realtime_composer->updateDetectionResults(current_stream_id, last_results);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        idx = (idx + 1) % handled_streams.size();
    }

    std::cout << "[推理线程 NPU " << npu_thread_id << "] 退出." << std::endl;
}
