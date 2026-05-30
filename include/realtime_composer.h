#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include <opencv2/opencv.hpp>

#include "common.h"
#include "rknn_detector.h"

/**
 * @brief 实时四宫格合成器
 *
 * 核心设计：
 * 1. 共享画面缓冲区（1280x960 BGR）
 * 2. 每路独立更新自己的区域（640x480）
 * 3. 推流线程定时读取当前画面
 * 4. 推理结果异步叠加
 *
 * 优势：
 * - 推流线程完全不阻塞
 * - 每路视频实时更新
 * - 推理结果异步叠加，有就显示，没有就显示原始画面
 */
class RealtimeComposer {
public:
    RealtimeComposer();
    ~RealtimeComposer();

    /**
     * @brief 初始化合成器
     * @param grid_width 四宫格宽度（默认1280）
     * @param grid_height 四宫格高度（默认960）
     */
    void init(int grid_width = 1280, int grid_height = 960);

    /**
     * @brief 更新某路视频帧（非阻塞）
     * @param stream_id 流ID（0-3）
     * @param frame BGR格式的视频帧（640x480）
     */
    void updateFrame(int stream_id, const cv::Mat& frame);

    /**
     * @brief 更新某路检测结果（非阻塞）
     * @param stream_id 流ID（0-3）
     * @param results 检测结果
     */
    void updateDetectionResults(int stream_id, const std::vector<DetectResult>& results);

    /**
     * @brief 获取当前四宫格画面（非阻塞）
     * @return 当前画面的拷贝
     */
    cv::Mat getCurrentGrid();

    /**
     * @brief 获取当前四宫格画面（零拷贝，需要外部加锁）
     * @return 画面引用
     */
    const cv::Mat& getCurrentGridRef() const { return grid_; }

    /**
     * @brief 获取画面锁（用于零拷贝场景）
     */
    std::mutex& getMutex() { return mutex_; }

private:
    /**
     * @brief 在画面上绘制检测结果
     * @param grid 目标画面
     * @param stream_id 流ID
     * @param results 检测结果
     */
    void drawDetections(cv::Mat& grid, int stream_id, const std::vector<DetectResult>& results);

    cv::Mat grid_;                          // 共享画面缓冲区
    std::mutex mutex_;                      // 画面锁
    int grid_width_ = 1280;
    int grid_height_ = 960;

    // 每路的ROI区域
    cv::Rect rois_[4];

    // 每路的检测结果（异步更新）
    std::vector<DetectResult> detections_[4];
    std::mutex detection_mutexes_[4];

    // 每路是否有新帧
    std::atomic<bool> has_new_frame_[4];
};
