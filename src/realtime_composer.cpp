#include "realtime_composer.h"

#include <iostream>

RealtimeComposer::RealtimeComposer() {
    // 初始化atomic数组
    for (int i = 0; i < 4; ++i) {
        has_new_frame_[i].store(false);
    }
}

RealtimeComposer::~RealtimeComposer() = default;

void RealtimeComposer::init(int grid_width, int grid_height) {
    grid_width_ = grid_width;
    grid_height_ = grid_height;

    // 创建共享画面缓冲区
    grid_ = cv::Mat(grid_height, grid_width, CV_8UC3, cv::Scalar(0, 0, 0));

    // 计算每路的ROI区域（四宫格布局）
    int cell_width = grid_width / 2;
    int cell_height = grid_height / 2;

    rois_[0] = cv::Rect(0, 0, cell_width, cell_height);                    // 左上
    rois_[1] = cv::Rect(cell_width, 0, cell_width, cell_height);           // 右上
    rois_[2] = cv::Rect(0, cell_height, cell_width, cell_height);          // 左下
    rois_[3] = cv::Rect(cell_width, cell_height, cell_width, cell_height); // 右下

    std::cout << "[实时合成器] 初始化完成: " << grid_width << "x" << grid_height
              << ", 每路区域: " << cell_width << "x" << cell_height << std::endl;
}

void RealtimeComposer::updateFrame(int stream_id, const cv::Mat& frame) {
    if (stream_id < 0 || stream_id >= 4 || frame.empty()) return;

    // 获取锁，更新对应区域
    std::lock_guard<std::mutex> lock(mutex_);

    // 将帧缩放并复制到对应ROI区域
    cv::Mat roi = grid_(rois_[stream_id]);
    cv::resize(frame, roi, rois_[stream_id].size());

    has_new_frame_[stream_id] = true;
}

void RealtimeComposer::updateDetectionResults(int stream_id, const std::vector<DetectResult>& results) {
    if (stream_id < 0 || stream_id >= 4) return;

    // 更新检测结果
    {
        std::lock_guard<std::mutex> lock(detection_mutexes_[stream_id]);
        detections_[stream_id] = results;
    }

    // 在画面上绘制检测结果
    std::lock_guard<std::mutex> lock(mutex_);
    drawDetections(grid_, stream_id, results);
}

cv::Mat RealtimeComposer::getCurrentGrid() {
    std::lock_guard<std::mutex> lock(mutex_);
    return grid_.clone();
}

void RealtimeComposer::drawDetections(cv::Mat& grid, int stream_id, const std::vector<DetectResult>& results) {
    cv::Mat roi = grid(rois_[stream_id]);

    for (const auto& det : results) {
        // det.box 是归一化坐标（0~1），需要映射到ROI区域
        int x1 = static_cast<int>(det.box.x * rois_[stream_id].width);
        int y1 = static_cast<int>(det.box.y * rois_[stream_id].height);
        int x2 = x1 + static_cast<int>(det.box.width * rois_[stream_id].width);
        int y2 = y1 + static_cast<int>(det.box.height * rois_[stream_id].height);

        // 绘制检测框
        cv::rectangle(roi, cv::Point(x1, y1), cv::Point(x2, y2),
                      cv::Scalar(0, 255, 0), 2);

        // 绘制标签
        std::string label = "class:" + std::to_string(det.classId) + " " + cv::format("%.2f", det.confidence);
        cv::putText(roi, label, cv::Point(x1, y1 - 5),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 2);
    }
}
