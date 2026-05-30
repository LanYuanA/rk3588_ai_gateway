#pragma once

#include <string>
#include <thread>
#include <vector>

#include "app_runtime.h"
#include "npu_pool.h"

struct WorkerThreads
{
    std::vector<std::thread> pullers; // 拉流线程数组
    std::vector<std::thread> npu_workers; // NPU工作线程数组
    std::vector<std::thread> dispatchers; // 推理分发线程数组
    std::thread streamer; // 推流线程
};

void startWorkerThreads(const std::vector<std::string> &stream_sources,
                        const std::string &model_path,
                        const RuntimeOptions &options,
                        WorkerThreads &workers);

void requestShutdown();

void joinWorkerThreads(WorkerThreads &workers);
