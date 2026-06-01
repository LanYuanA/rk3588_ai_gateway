#include "realtime_composer.h"

#include <iostream>

RealtimeComposer::RealtimeComposer() = default;

RealtimeComposer::~RealtimeComposer() = default;

void RealtimeComposer::init(int grid_width, int grid_height) {
    grid_width_ = grid_width;
    grid_height_ = grid_height;

    // 计算每路的ROI区域（四宫格布局）
    int cell_width = grid_width / 2;
    int cell_height = grid_height / 2;

    rois_[0] = cv::Rect(0, 0, cell_width, cell_height);                    // 左上
    rois_[1] = cv::Rect(cell_width, 0, cell_width, cell_height);           // 右上
    rois_[2] = cv::Rect(0, cell_height, cell_width, cell_height);          // 左下
    rois_[3] = cv::Rect(cell_width, cell_height, cell_width, cell_height); // 右下

    // 初始化每路的帧缓冲区
    for (int i = 0; i < 4; i++) {
        frames_[i] = cv::Mat(cell_height, cell_width, CV_8UC3, cv::Scalar(0, 0, 0));
    }

    std::cout << "[实时合成器] 初始化完成: " << grid_width << "x" << grid_height
              << ", 每路区域: " << cell_width << "x" << cell_height << std::endl;
}

void RealtimeComposer::updateFrame(int stream_id, const cv::Mat& frame) {
    if (stream_id < 0 || stream_id >= 4 || frame.empty()) return;

    // 只更新原始帧缓冲区（不绘制检测框）
    std::lock_guard<std::mutex> lock(frame_mutex_);
    cv::resize(frame, frames_[stream_id], rois_[stream_id].size());
}

void RealtimeComposer::updateDetectionResults(int stream_id, const std::vector<DetectResult>& results) {
    if (stream_id < 0 || stream_id >= 4) return;

    // 保存检测结果
    {
        std::lock_guard<std::mutex> lock(detection_mutexes_[stream_id]);
        detections_[stream_id] = results;
    }
}

cv::Mat RealtimeComposer::getCurrentGrid() {
    // 一次性合成四宫格：读取原始帧 + 叠加检测框
    cv::Mat grid(grid_height_, grid_width_, CV_8UC3, cv::Scalar(0, 0, 0));

    std::lock_guard<std::mutex> frame_lock(frame_mutex_);

    for (int i = 0; i < 4; i++) {
        if (!frames_[i].empty()) {
            // 先复制原始帧到grid
            frames_[i].copyTo(grid(rois_[i]));

            // 再叠加检测结果
            std::vector<DetectResult> results;
            {
                std::lock_guard<std::mutex> det_lock(detection_mutexes_[i]);
                results = detections_[i];
            }
            if (!results.empty()) {
                drawDetections(grid, i, results);
            }
        }
    }

    return grid;
}

void RealtimeComposer::drawDetections(cv::Mat& grid, int stream_id, const std::vector<DetectResult>& results) {
    // 获取当前路的帧尺寸作为参考
    cv::Mat roi = grid(rois_[stream_id]);

    // 原始图像尺寸（推理输入是640x480）
    const float src_width = 640.0f;
    const float src_height = 480.0f;

    // 计算缩放比例：原始图像 → ROI区域
    float scale_x = static_cast<float>(rois_[stream_id].width) / src_width;
    float scale_y = static_cast<float>(rois_[stream_id].height) / src_height;

    for (const auto& det : results) {
        // det.box 是像素坐标（0~640），需要映射到ROI区域
        int x1 = static_cast<int>(det.box.x * scale_x);
        int y1 = static_cast<int>(det.box.y * scale_y);
        int x2 = x1 + static_cast<int>(det.box.width * scale_x);
        int y2 = y1 + static_cast<int>(det.box.height * scale_y);

        // 边界检查
        x1 = std::max(0, std::min(x1, rois_[stream_id].width - 1));
        y1 = std::max(0, std::min(y1, rois_[stream_id].height - 1));
        x2 = std::max(0, std::min(x2, rois_[stream_id].width - 1));
        y2 = std::max(0, std::min(y2, rois_[stream_id].height - 1));

        // 绘制检测框
        cv::rectangle(roi, cv::Point(x1, y1), cv::Point(x2, y2),
                      cv::Scalar(0, 255, 0), 2);

        // 绘制标签
        std::string label = cv::format("Face %.2f", det.confidence);
        cv::putText(roi, label, cv::Point(x1, y1 - 5),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 2);
    }
}
