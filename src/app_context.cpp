#include "app_context.h"

std::atomic<bool> g_system_running(true);
std::vector<DetectResult> g_latest_results[NUM_STREAMS];
std::mutex g_results_mutex[NUM_STREAMS];
ThreadSafeQueue<VideoFrame> g_inference_queues[NUM_STREAMS];
ThreadSafeQueue<VideoFrame> g_push_queues[NUM_STREAMS];
