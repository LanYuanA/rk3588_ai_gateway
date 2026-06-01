#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include <opencv2/opencv.hpp>

#include "common.h"
#include "rknn_detector.h"

/**
 * @brief 实时四宫格合成器（零拷贝版）
 *
 * 核心设计：
 * 1. 一块连续buffer（1280×960×3 BGR）
 * 2. 每路拉流线程直接写入buffer的对应ROI区域
 * 3. 推流线程直接读buffer地址，无需拷贝
 * 4. 检测框直接画在buffer上
 *
 * 内存布局：
 * ┌──────────┬──────────┐
 * │ stream 0 │ stream 1 │  ← 每路直接写入
 * │ (0,0)    │ (640,0)  │
 * ├──────────┼──────────┤
 * │ stream 2 │ stream 3 │
 * │ (0,480)  │ (640,480)│
 * └──────────┴──────────┘
 *    1280 × 960
 */
class RealtimeComposer {
public:
    RealtimeComposer();
    ~RealtimeComposer();

    void init(int grid_width = 1280, int grid_height = 960);

    /**
     * @brief 更新某路视频帧（直接写入grid buffer对应区域）
     */
    void updateFrame(int stream_id, const cv::Mat& frame);

    /**
     * @brief 更新某路检测结果（直接在grid buffer上画框）
     */
    void updateDetectionResults(int stream_id, const std::vector<DetectResult>& results);

    /**
     * @brief 获取当前四宫格画面（零拷贝，返回grid buffer的clone）
     */
    cv::Mat getCurrentGrid();

    /**
     * @brief 获取grid buffer的cv::Mat包装（不拷贝数据）
     *        推流线程可以直接读取data指针
     */
    cv::Mat getCurrentGridRef();

    std::mutex& getMutex() { return buffer_mutex_; }

private:
    void drawDetections(cv::Mat& roi, int stream_id, const std::vector<DetectResult>& results);

    // 一块连续的grid buffer
    uint8_t* grid_buffer_ = nullptr;
    cv::Mat grid_mat_;          // 包装grid_buffer_的cv::Mat（不拥有数据）

    int grid_width_ = 1280;
    int grid_height_ = 960;

    // 每路的ROI区域（在grid中的位置）
    cv::Rect rois_[4];

    // 每路的检测结果
    std::vector<DetectResult> detections_[4];
    std::mutex detection_mutexes_[4];
    std::mutex buffer_mutex_;   // 保护grid_buffer_的写入
};
