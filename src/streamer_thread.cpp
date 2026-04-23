#include "streamer_thread.h"

#include <chrono>
#include <array>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <thread>
#include <vector>

#include "app_context.h"
#include "stitcher.h"

void streamerThread() {
    std::cout << "[推流线程] 启动，初始化 4 路融合 + GStreamer MPP 硬件编码..." << std::endl;

    std::string push_rtsp =
        "appsrc is-live=true block=false "
        "! videoconvert "
        "! video/x-raw,format=NV12,width=1280,height=960,framerate=30/1 "
        "! mpph264enc "
        "! h264parse config-interval=1 "
        "! rtspclientsink location=rtsp://127.0.0.1:8554/gateway_out protocols=tcp";

    cv::VideoWriter writer(push_rtsp, cv::CAP_GSTREAMER, 0, 30.0, cv::Size(1280, 960), true);

    if (!writer.isOpened()) {
        std::cerr << "[推流线程-警告] 无法启动基础录制，尝试退火到无硬件加速的 OpenCV RAW 写入！" << std::endl;
        writer.open("debug_4ch_output.avi", cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 15.0, cv::Size(1280, 960), true);
        if (!writer.isOpened()) {
            std::cerr << "[推流线程-警告] 无法启动任何本地录制！" << std::endl;
        }
    }

    std::array<VideoFrame, NUM_STREAMS> latest_frames{};
    int frame_count = 0;

    while (g_system_running) {
        updateCanvasFromPushQueues(latest_frames);

        bool has_valid_frame = false;
        for (const auto& frame : latest_frames) {
            if (!frame.image.empty()) {
                has_valid_frame = true;
                break;
            }
        }

        if (!has_valid_frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (writer.isOpened()) {
            auto t1_merge = std::chrono::steady_clock::now();
            cv::Mat final_grid = composeGridWithDetections(latest_frames, 0); // 移除时间戳参数
            auto t2_merge = std::chrono::steady_clock::now();
            writer.write(final_grid);
            auto t3_write = std::chrono::steady_clock::now();

            frame_count++;
            if (frame_count % 30 == 0) {
                int merge_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2_merge - t1_merge).count();
                int write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t3_write - t2_merge).count();
                std::cout << "[四路流-推流线程] 已推送第 " << frame_count
                          << " 帧。 拼图耗时: " << merge_ms
                          << " ms，写入耗时: " << write_ms << " ms" << std::endl;
            }
            
            // 使用更灵活的延迟机制，基于实际处理时间来控制帧率
            auto sleep_start = std::chrono::steady_clock::now();
            auto sleep_duration = std::chrono::milliseconds(33) - (std::chrono::steady_clock::now() - t3_write);
            if (sleep_duration.count() > 0) {
                std::this_thread::sleep_for(sleep_duration);
            }
        } else {
            // 如果writer未打开，仍然保持一定的延迟
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    }

    if (writer.isOpened()) {
        writer.release();
    }
    std::cout << "[推流线程] 退出并安全释放硬件写入句柄." << std::endl;
}
