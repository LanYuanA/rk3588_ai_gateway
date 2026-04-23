#include "stitcher.h"

#include <array>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>

#include "app_context.h"

void updateCanvasFromPushQueues(std::array<VideoFrame, NUM_STREAMS>& latest_frames) {
    VideoFrame frame;
    for (int i = 0; i < NUM_STREAMS; ++i) {
        // 清空队列中除了最新帧外的所有帧，确保获取到最新的视频帧
        while (g_push_queues[i].size() > 1) {
            VideoFrame dummy;
            g_push_queues[i].pop(dummy);
        }
        // 获取最新帧
        if (g_push_queues[i].size() > 0 && g_push_queues[i].pop(frame)) {
            latest_frames[i] = frame;
        }
    }
}

cv::Mat composeGridWithDetections(const std::array<VideoFrame, NUM_STREAMS>& latest_frames,
                                  int64_t unused_sync_reference) {
    cv::Mat final_grid(960, 1280, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Rect rois[4] = {
        cv::Rect(0, 0, 640, 480),
        cv::Rect(640, 0, 640, 480),
        cv::Rect(0, 480, 640, 480),
        cv::Rect(640, 480, 640, 480)
    };

    for (int i = 0; i < NUM_STREAMS; ++i) {
        if (!latest_frames[i].image.empty()) {
            cv::Mat roi_mat = final_grid(rois[i]);
            latest_frames[i].image.copyTo(roi_mat);

            // 快速获取检测结果，不等待推理线程
            std::vector<DetectResult> current_res;
            if (g_results_mutex[i].try_lock()) {
                current_res = g_latest_results[i];
                g_results_mutex[i].unlock();
            } // 如果无法立即获得锁，则使用空的current_res，即不显示检测框

            // 显示简单的帧信息，不包含时间戳同步信息
            std::ostringstream info_text;
            info_text << "Stream: " << i;
            cv::putText(roi_mat,
                        info_text.str(),
                        cv::Point(10, 22),
                        cv::FONT_HERSHEY_SIMPLEX,
                        0.55,
                        cv::Scalar(255, 255, 0),
                        2);

            for (const auto& res : current_res) {
                cv::rectangle(roi_mat, res.box, cv::Scalar(0, 255, 0), 2);
                std::string label = "Face: " + std::to_string(res.confidence).substr(0, 4);
                cv::putText(roi_mat,
                            label,
                            cv::Point(res.box.x, res.box.y - 5),
                            cv::FONT_HERSHEY_SIMPLEX,
                            0.5,
                            cv::Scalar(0, 255, 0),
                            1);
            }
        }
    }

    // 在最终网格的右下角显示当前实时时间（精确到秒）
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_time), "%H:%M:%S");

    cv::putText(final_grid,
                "Time: " + ss.str(),
                cv::Point(final_grid.cols - 200, final_grid.rows - 20),
                cv::FONT_HERSHEY_SIMPLEX,
                0.6,
                cv::Scalar(0, 255, 255),
                2);

    return final_grid;
}
