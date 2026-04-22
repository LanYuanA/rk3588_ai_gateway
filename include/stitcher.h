#pragma once

#include <opencv2/opencv.hpp>
#include <vector>

void updateCanvasFromPushQueues(std::vector<cv::Mat>& canvas);
cv::Mat composeGridWithDetections(const std::vector<cv::Mat>& canvas);
