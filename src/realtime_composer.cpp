#include "realtime_composer.h"

#include <iostream>
#include <cstring>

RealtimeComposer::RealtimeComposer() = default;

RealtimeComposer::~RealtimeComposer() {
    if (grid_buffer_) {
        delete[] grid_buffer_;
        grid_buffer_ = nullptr;
    }
}

void RealtimeComposer::init(int grid_width, int grid_height) {
    grid_width_ = grid_width;
    grid_height_ = grid_height;

    // 分配一块连续buffer
    grid_buffer_ = new uint8_t[grid_width * grid_height * 3];
    std::memset(grid_buffer_, 0, grid_width * grid_height * 3);

    // 用cv::Mat包装这块buffer（不拥有数据，不拷贝）
    grid_mat_ = cv::Mat(grid_height, grid_width, CV_8UC3, grid_buffer_);

    // 计算每路的ROI区域
    int cell_w = grid_width / 2;
    int cell_h = grid_height / 2;
    rois_[0] = cv::Rect(0, 0, cell_w, cell_h);
    rois_[1] = cv::Rect(cell_w, 0, cell_w, cell_h);
    rois_[2] = cv::Rect(0, cell_h, cell_w, cell_h);
    rois_[3] = cv::Rect(cell_w, cell_h, cell_w, cell_h);

    std::cout << "[实时合成器] 初始化完成: " << grid_width << "x" << grid_height
              << " (连续buffer " << grid_width * grid_height * 3 << " 字节)" << std::endl;
}

void RealtimeComposer::updateFrame(int stream_id, const cv::Mat& frame) {
    if (stream_id < 0 || stream_id >= 4 || frame.empty()) return;

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // 直接写入grid buffer的对应ROI区域（零拷贝目标）
    cv::Mat roi_dst = grid_mat_(rois_[stream_id]);
    cv::resize(frame, roi_dst, rois_[stream_id].size());
}

void RealtimeComposer::updateDetectionResults(int stream_id, const std::vector<DetectResult>& results) {
    if (stream_id < 0 || stream_id >= 4) return;

    // 保存检测结果
    {
        std::lock_guard<std::mutex> lock(detection_mutexes_[stream_id]);
        detections_[stream_id] = results;
    }

    // 直接在grid buffer上绘制检测框
    if (!results.empty()) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        cv::Mat roi = grid_mat_(rois_[stream_id]);
        drawDetections(roi, stream_id, results);
    }
}

cv::Mat RealtimeComposer::getCurrentGrid() {
    // 返回clone（推流线程用完后释放）
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return grid_mat_.clone();
}

cv::Mat RealtimeComposer::getCurrentGridRef() {
    // 零拷贝：返回grid_mat_引用（推流线程必须在锁内使用）
    return grid_mat_;
}

void RealtimeComposer::drawDetections(cv::Mat& roi, int stream_id, const std::vector<DetectResult>& results) {
    const float src_width = 640.0f;
    const float src_height = 480.0f;
    float scale_x = static_cast<float>(rois_[stream_id].width) / src_width;
    float scale_y = static_cast<float>(rois_[stream_id].height) / src_height;

    for (const auto& det : results) {
        int x1 = static_cast<int>(det.box.x * scale_x);
        int y1 = static_cast<int>(det.box.y * scale_y);
        int x2 = x1 + static_cast<int>(det.box.width * scale_x);
        int y2 = y1 + static_cast<int>(det.box.height * scale_y);

        x1 = std::max(0, std::min(x1, rois_[stream_id].width - 1));
        y1 = std::max(0, std::min(y1, rois_[stream_id].height - 1));
        x2 = std::max(0, std::min(x2, rois_[stream_id].width - 1));
        y2 = std::max(0, std::min(y2, rois_[stream_id].height - 1));

        cv::rectangle(roi, cv::Point(x1, y1), cv::Point(x2, y2),
                      cv::Scalar(0, 255, 0), 2);
        std::string label = cv::format("Face %.2f", det.confidence);
        cv::putText(roi, label, cv::Point(x1, y1 - 5),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 2);
    }
}
