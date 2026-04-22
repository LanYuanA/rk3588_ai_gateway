#pragma once

#include <string>
#include <thread>
#include <vector>

#include "app_runtime.h"

struct WorkerThreads {
    std::vector<std::thread> pullers;
    std::thread infer1;
    std::thread infer2;
    std::thread streamer;
};

void startWorkerThreads(const std::vector<std::string>& stream_sources,
                        const std::string& model_path,
                        const RuntimeOptions& options,
                        WorkerThreads& workers);

void requestShutdown();

void joinWorkerThreads(WorkerThreads& workers);
