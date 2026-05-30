#pragma once

#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <opencv2/opencv.hpp>

// 定义视频帧数据结构体
// 使用 shared_ptr 共享图像数据，推理和推流线程读同一块内存，避免 clone
struct VideoFrame {
    int stream_id;      // 流的编号 (0~3)
    uint64_t frame_id;  // 帧序号
    int64_t timestamp_ms = 0; // 时间戳字段保留，但不再使用
    std::shared_ptr<cv::Mat> image;  // 共享的图像数据（引用计数，零拷贝）
};

// 系统运行状态标志
extern std::atomic<bool> g_system_running;
