#include "app_lifecycle.h"

#include <iostream>

#include "app_context.h"
#include "app_thread_utils.h"
#include "inference_thread.h"
#include "puller_thread.h"
#include "streamer_thread.h"

void startWorkerThreads(const std::vector<std::string>& stream_sources,
                        const std::string& model_path,
                        const RuntimeOptions& options,
                        WorkerThreads& workers) {
    workers.pullers.clear();
    workers.pullers.reserve(NUM_STREAMS);

    for (int i = 0; i < NUM_STREAMS; ++i) {
        workers.pullers.emplace_back(streamPullerAndDecoderThread, i, stream_sources[i]);
        if (options.pin_threads) {
            bindThreadToCpu(workers.pullers.back(), i % 4, "puller-" + std::to_string(i));
        }
    }

    workers.infer1 = std::thread(inferenceThread, model_path, std::vector<int>{0, 1}, 1, 0);
    workers.infer2 = std::thread(inferenceThread, model_path, std::vector<int>{2, 3}, 2, 1);
    if (options.pin_threads) {
        bindThreadToCpu(workers.infer1, 4, "infer-1");
        bindThreadToCpu(workers.infer2, 5, "infer-2");
    }

    workers.streamer = std::thread(streamerThread);
    if (options.pin_threads) {
        bindThreadToCpu(workers.streamer, 6, "streamer");
    }
}

void requestShutdown() {
    std::cout << "正在通知所有线程退出...\n";
    g_system_running = false;
    for (int i = 0; i < NUM_STREAMS; ++i) {
        g_inference_queues[i].wake_up_all();
    }
}

void joinWorkerThreads(WorkerThreads& workers) {
    for (auto& t : workers.pullers) {
        if (t.joinable()) {
            t.join();
        }
    }
    if (workers.infer1.joinable()) {
        workers.infer1.join();
    }
    if (workers.infer2.joinable()) {
        workers.infer2.join();
    }
    if (workers.streamer.joinable()) {
        workers.streamer.join();
    }
}
