#pragma once

#include <cstdint>
#include <array>
#include <opencv2/opencv.hpp>
#include <vector>

#include "app_context.h"
#include "common.h"

void updateCanvasFromPushQueues(std::array<VideoFrame, NUM_STREAMS>& latest_frames);
cv::Mat composeGridWithDetections(const std::array<VideoFrame, NUM_STREAMS>& latest_frames,
								  int64_t sync_reference_ms);

// 使用 RGA 硬件将 BGR 四宫格转换为 NV12，替代 GStreamer videoconvert (CPU)
// 重载1: 输出到 cv::Mat（内部会 memcpy）
bool convertGridBgrToNv12WithRga(const cv::Mat& bgr_grid, cv::Mat& nv12_out);
// 重载2: 直接写入目标缓冲区（零拷贝，推荐用于 GStreamer 推流）
bool convertGridBgrToNv12WithRga(const cv::Mat& bgr_grid, void* dst_buf, size_t dst_capacity);
