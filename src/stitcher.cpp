#include "stitcher.h"

#include <mutex>
#include <string>
#include <vector>

#include "app_context.h"

void updateCanvasFromPushQueues(std::vector<cv::Mat>& canvas) {
    VideoFrame frame;
    for (int i = 0; i < NUM_STREAMS; ++i) {
        while (g_push_queues[i].size() > 1) {
            VideoFrame dummy;
            g_push_queues[i].pop(dummy);
        }
        if (g_push_queues[i].size() > 0 && g_push_queues[i].pop(frame)) {
            canvas[i] = frame.image;
        }
    }
}

cv::Mat composeGridWithDetections(const std::vector<cv::Mat>& canvas) {
    cv::Mat final_grid(960, 1280, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Rect rois[4] = {
        cv::Rect(0, 0, 640, 480),
        cv::Rect(640, 0, 640, 480),
        cv::Rect(0, 480, 640, 480),
        cv::Rect(640, 480, 640, 480)
    };

    for (int i = 0; i < NUM_STREAMS; ++i) {
        if (!canvas[i].empty()) {
            cv::Mat roi_mat = final_grid(rois[i]);
            canvas[i].copyTo(roi_mat);

            std::vector<DetectResult> current_res;
            {
                std::lock_guard<std::mutex> lock(g_results_mutex[i]);
                current_res = g_latest_results[i];
            }

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

    return final_grid;
}
