#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "ThreadSafeQueue.h"
#include "common.h"
#include "rknn_detector.h"
#include "npu_pool.h"

class RealtimeComposer;

constexpr int NUM_STREAMS = 4;

extern std::vector<DetectResult> g_latest_results[NUM_STREAMS];
extern std::mutex g_results_mutex[NUM_STREAMS];
extern ThreadSafeQueue<VideoFrame> g_inference_queues[NUM_STREAMS];
extern ThreadSafeQueue<VideoFrame> g_push_queues[NUM_STREAMS];

// 全局实时合成器（非阻塞架构）
extern RealtimeComposer* g_realtime_composer;
