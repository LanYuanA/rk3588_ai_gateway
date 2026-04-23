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
