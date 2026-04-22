#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "ThreadSafeQueue.h"
#include "common.h"
#include "rknn_detector.h"

constexpr int NUM_STREAMS = 4;

extern std::vector<DetectResult> g_latest_results[NUM_STREAMS];
extern std::mutex g_results_mutex[NUM_STREAMS];
extern ThreadSafeQueue<VideoFrame> g_inference_queues[NUM_STREAMS];
extern ThreadSafeQueue<VideoFrame> g_push_queues[NUM_STREAMS];
