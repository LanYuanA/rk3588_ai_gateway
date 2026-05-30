#include "app_context.h"
#include "realtime_composer.h"

std::atomic<bool> g_system_running(true);
std::vector<DetectResult> g_latest_results[NUM_STREAMS];
std::mutex g_results_mutex[NUM_STREAMS];
ThreadSafeQueue<VideoFrame> g_inference_queues[NUM_STREAMS];
ThreadSafeQueue<VideoFrame> g_push_queues[NUM_STREAMS];

// 全局实时合成器（非阻塞架构）
RealtimeComposer* g_realtime_composer = nullptr;
