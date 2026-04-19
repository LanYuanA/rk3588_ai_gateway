import re

with open("src/main.cpp", "r") as f:
    code = f.read()

# 1. Add global variables for latest async box results
header_addition = """#include "rknn_detector.h"
#include <mutex>
#include <signal.h>

std::vector<DetectResult> g_latest_results[NUM_STREAMS];
std::mutex g_results_mutex[NUM_STREAMS];
"""
code = code.replace('#include "rknn_detector.h"\n#include <signal.h>', header_addition)

# 2. Modify pulling thread
push_addition = """        // 将解码后的帧推入对应流的缓冲队列(给 NPU 使用)
        g_inference_queues[streamId].push(frame);
        
        // 核心解耦：直接把纯净的原始画面送给推流线程画板，画面不等 NPU！
        g_push_queue.push(frame);
"""
code = code.replace('        // 将解码后的帧推入对应流的缓冲队列\n        g_inference_queues[streamId].push(frame);', push_addition)


# 3. Modify inferenceThread to accept a list of streams and update global results
infer_old = """void inferenceThread(const std::string& model_path) {
    std::cout << "[推理线程 NPU] 启动，准备加载模型..." << std::endl;
    
    // 初始化 NPU 引擎并分配硬件资源
    RKNNDetector detector;
    if (!detector.init(model_path)) {
        std::cerr << "[错误] NPU 模型初始化失败，推理队列结束。" << std::endl;
        return;
    }

    int current_stream_id = 0;
    while (g_system_running) {
        // 轮询获取各路流的数据 (也可以用多线程NPU处理)
        VideoFrame frame;
        
        // 我们改为通过 pop(frame) 阻塞等待，如果队列空则一直等，有数据才提取
        // 这样 NPU 线程就不会在有堆积时被我们简单的 if 逻辑给忽略了
        if (g_inference_queues[current_stream_id].size() > 0) {
            
            // ================= 核心修复：防止堆积延迟（防“10秒变2秒”慢动作）=================
            // 四路摄像头如果在以 30FPS 狂塞数据，而当前 NPU 因为只有1个核心（或者受算力限制）总处理时长大于 33ms，
            // 必然会导致列队无限堆积。我们取数据时，如果发现队列里有多于 1 帧的堆积积压数据，直接全部抛弃， 只拿最新的一帧！
            while (g_inference_queues[current_stream_id].size() > 1) {
                VideoFrame dummy;
                g_inference_queues[current_stream_id].pop(dummy);
            }
            // =========================================================================

            if (g_inference_queues[current_stream_id].pop(frame)) {
                // std::cout << "[NPU 线程] 提取 流 " << frame.stream_id 
                //           << " 的第 " << frame.frame_id << " 帧进行推理..." << std::endl;
                
                // 将真实图像抛给底层的 RKNNDetector，发起 C API 对硬件的调用
                std::vector<DetectResult> results = detector.inference(frame.image);
                
                // 在图像上绘制人脸检测框
                for (const auto& res : results) {
                    cv::rectangle(frame.image, res.box, cv::Scalar(0, 255, 0), 2);
                    std::string label = "Face: " + std::to_string(res.confidence).substr(0, 4);
                    cv::putText(frame.image, label, cv::Point(res.box.x, res.box.y - 5), 
                                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
                }

                // 将已被 NPU 渲染好的人脸框画面压入后端的推流队列，交由推流线程送出
                g_push_queue.push(frame);
            }
        }
        
        // 循环切换处理下一路视频流
        current_stream_id = (current_stream_id + 1) % NUM_STREAMS;
        
        // 防止 CPU 空转
        if (current_stream_id == 0) {
           std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::cout << "[推理线程 NPU] 退出." << std::endl;
}"""

infer_new = """void inferenceThread(const std::string& model_path, std::vector<int> handled_streams, int npu_thread_id) {
    if (handled_streams.empty()) return;
    
    std::cout << "[推理线程 NPU " << npu_thread_id << "] 启动，接管流: ";
    for (int s : handled_streams) std::cout << s << " ";
    std::cout << "准备加载模型..." << std::endl;
    
    RKNNDetector detector;
    if (!detector.init(model_path)) {
        std::cerr << "[错误] NPU " << npu_thread_id << " 模型初始化失败。" << std::endl;
        return;
    }

    int idx = 0;
    while (g_system_running) {
        int current_stream_id = handled_streams[idx];
        VideoFrame frame;
        
        if (g_inference_queues[current_stream_id].size() > 0) {
            // 防积压：抽水机式扔旧帧
            while (g_inference_queues[current_stream_id].size() > 1) {
                VideoFrame dummy;
                g_inference_queues[current_stream_id].pop(dummy);
            }

            if (g_inference_queues[current_stream_id].pop(frame)) {
                std::vector<DetectResult> results = detector.inference(frame.image);
                
                // 将推理出来的结果坐标异步写到全局变量里供推流画板直接取用
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
}"""
code = code.replace(infer_old, infer_new)

# 4. Modify streamerThread to draw async results
canvas_drawing_old = """                cv::Mat top_row, bottom_row, final_grid;
                cv::hconcat(canvas[0], canvas[1], top_row);
                cv::hconcat(canvas[2], canvas[3], bottom_row);
                cv::vconcat(top_row, bottom_row, final_grid);"""

canvas_drawing_new = """                cv::Mat top_row, bottom_row, final_grid;
                
                // 在拼接前，给各路纯展示画面加上最新已知的识别框（异步解耦）
                std::vector<cv::Mat> display_canvas(NUM_STREAMS);
                for (int i = 0; i < NUM_STREAMS; ++i) {
                    display_canvas[i] = canvas[i].clone(); // 克隆一次防污染纯净底片
                    std::vector<DetectResult> current_res;
                    {
                        std::lock_guard<std::mutex> lock(g_results_mutex[i]);
                        current_res = g_latest_results[i]; // 立刻提取最新的结果
                    }
                    
                    // 将最新识别框直接在这个准备推流的副本上绘制
                    for (const auto& res : current_res) {
                        cv::rectangle(display_canvas[i], res.box, cv::Scalar(0, 255, 0), 2);
                        std::string label = "Face: " + std::to_string(res.confidence).substr(0, 4);
                        cv::putText(display_canvas[i], label, cv::Point(res.box.x, res.box.y - 5), 
                                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
                    }
                }

                cv::hconcat(display_canvas[0], display_canvas[1], top_row);
                cv::hconcat(display_canvas[2], display_canvas[3], bottom_row);
                cv::vconcat(top_row, bottom_row, final_grid);"""
code = code.replace(canvas_drawing_old, canvas_drawing_new)


# 5. Modify main() to launch two inference threads
main_old = """    std::string model_path = "/home/kylin/kylin/process/yolov8_face_fp.rknn"; // 修改为您真实的模型名称
    std::thread infer(inferenceThread, model_path);

    // 3. 启动拼接与推流线程"""
main_new = """    std::string model_path = "/home/kylin/kylin/process/yolov8_face_fp.rknn"; // 修改为您真实的模型名称
    
    // 使用独立的多核 NPU 线程！上半部分给 NPU 0 跑，下半部分给 NPU 1 跑！
    std::thread infer1(inferenceThread, model_path, std::vector<int>{0, 1}, 1);
    std::thread infer2(inferenceThread, model_path, std::vector<int>{2, 3}, 2);

    // 3. 启动拼接与推流线程"""
code = code.replace(main_old, main_new)

main_join_old = """    if (infer.joinable()) infer.join();"""
main_join_new = """    if (infer1.joinable()) infer1.join();
    if (infer2.joinable()) infer2.join();"""
code = code.replace(main_join_old, main_join_new)

with open("src/main.cpp", "w") as f:
    f.write(code)
print("Patch applied successfully.")
