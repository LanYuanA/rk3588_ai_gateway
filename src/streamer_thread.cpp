#include "streamer_thread.h"

#include <chrono>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <thread>
#include <vector>

#include "app_context.h"
#include "stitcher.h"

void streamerThread() {
    std::cout << "[推流线程] 启动，初始化 4 路融合 + GStreamer MPP 硬件编码..." << std::endl;

    std::string push_rtsp =
        "appsrc is-live=true do-timestamp=true format=time block=false "
        "! queue leaky=downstream max-size-buffers=2 "
        "! video/x-raw,format=BGR,width=1280,height=960,framerate=30/1 "
        "! videoconvert "
        "! queue leaky=downstream max-size-buffers=2 "
        "! mpph264enc bps=4000000 rc-mode=cbr gop=60 "
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

    std::vector<cv::Mat> canvas(NUM_STREAMS, cv::Mat::zeros(480, 640, CV_8UC3));
    int frame_count = 0;
    auto last_write_time = std::chrono::steady_clock::now();

    while (g_system_running) {
        updateCanvasFromPushQueues(canvas);

        auto now = std::chrono::steady_clock::now();
        auto ms_passed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_write_time).count();

        if (ms_passed >= 33) {
            if (writer.isOpened()) {
                auto t1_merge = std::chrono::steady_clock::now();
                cv::Mat final_grid = composeGridWithDetections(canvas);
                auto t2_merge = std::chrono::steady_clock::now();
                writer.write(final_grid);
                auto t3_write = std::chrono::steady_clock::now();

                frame_count++;
                if (frame_count % 30 == 0) {
                    int merge_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2_merge - t1_merge).count();
                    int write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t3_write - t2_merge).count();
                    std::cout << "[四路流-推流线程] 已同步推送第 " << frame_count
                              << " 帧。 前期拼图耗时: " << merge_ms
                              << " ms，GStreamer编码写入耗时: " << write_ms << " ms" << std::endl;
                }
            }

            last_write_time += std::chrono::milliseconds(33);
            if (std::chrono::steady_clock::now() - last_write_time > std::chrono::milliseconds(100)) {
                last_write_time = std::chrono::steady_clock::now();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    if (writer.isOpened()) {
        writer.release();
    }
    std::cout << "[推流线程] 退出并安全释放硬件写入句柄." << std::endl;
}
