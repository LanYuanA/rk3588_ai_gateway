#include "app_lifecycle.h"

#include <iostream>

#include "app_context.h"
#include "app_thread_utils.h"
#include "npu_pool.h"
#include "puller_thread.h"
#include "streamer_thread.h"

// NPU工作线程统计（每个核心一个）
static NpuWorkerStats g_npu_stats[NUM_NPU_CORES];

// 负责线程的创建以及初始调度，开工！
void startWorkerThreads(const std::vector<std::string> &stream_sources,
                        const std::string &model_path,
                        const RuntimeOptions &options,
                        WorkerThreads &workers)
{
    // 1. 启动拉流线程
    workers.pullers.clear();
    workers.pullers.reserve(NUM_STREAMS);
    for (int i = 0; i < NUM_STREAMS; ++i)
    {
        workers.pullers.emplace_back(streamPullerAndDecoderThread, i, stream_sources[i]);
        if (options.pin_threads)
        {
            bindThreadToCpu(workers.pullers.back(), i % 4, "puller-" + std::to_string(i));
        }
    }

    // 2. 启动NPU工作线程池（3个核心）
    workers.npu_workers.reserve(NUM_NPU_CORES);
    for (int i = 0; i < NUM_NPU_CORES; ++i)
    {
        workers.npu_workers.emplace_back(npuWorkerThread, model_path, i, std::ref(g_npu_stats[i]));
        if (options.pin_threads)
        {
            // NPU工作线程绑定到大核（核心4-6）
            bindThreadToCpu(workers.npu_workers.back(), 4 + (i % 3), "npu-worker-" + std::to_string(i));
        }
    }

    // 3. 启动推理分发线程（每个流一个）
    workers.dispatchers.reserve(NUM_STREAMS);
    for (int i = 0; i < NUM_STREAMS; ++i)
    {
        workers.dispatchers.emplace_back(inferenceDispatcherThread, i);
    }

    // 4. 启动推流线程
    workers.streamer = std::thread(streamerThread);
    if (options.pin_threads)
    {
        bindThreadToCpu(workers.streamer, 6, "streamer");
    }

    std::cout << "线程池初始化完成: "
              << NUM_STREAMS << "个拉流线程, "
              << NUM_NPU_CORES << "个NPU工作线程, "
              << NUM_STREAMS << "个推理分发线程, "
              << "1个推流线程" << std::endl;
}

void requestShutdown()
{
    std::cout << "正在通知所有线程退出...\n";
    g_system_running = false;
    for (int i = 0; i < 4; ++i)
    {
        // 唤醒所有阻塞的线程，该下班了
        g_inference_queues[i].wake_up_all();
        // 唤醒NPU任务队列
        g_npu_task_queues[i].wake_up_all();
    }
}

void joinWorkerThreads(WorkerThreads &workers)
{
    // 等待拉流线程结束
    for (auto &t : workers.pullers)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    // 等待NPU工作线程结束
    for (auto &t : workers.npu_workers)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    // 等待推理分发线程结束
    for (auto &t : workers.dispatchers)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    // 等待推流线程结束
    if (workers.streamer.joinable())
    {
        workers.streamer.join();
    }

    // 打印NPU统计信息
    std::cout << "\n===== NPU统计信息 =====" << std::endl;
    uint64_t total_tasks = 0;
    uint64_t total_latency = 0;
    for (int i = 0; i < 3; ++i)
    {
        uint64_t tasks = g_npu_stats[i].tasks_processed;
        uint64_t latency = g_npu_stats[i].total_latency_us;
        total_tasks += tasks;
        total_latency += latency;
        std::cout << "NPU核心 " << i << ": "
                  << "处理任务数=" << tasks
                  << ", 平均延迟=" << (tasks > 0 ? latency / tasks : 0) << "μs"
                  << std::endl;
    }
    std::cout << "总计: " << total_tasks << "个任务, "
              << "平均延迟=" << (total_tasks > 0 ? total_latency / total_tasks : 0) << "μs"
              << std::endl;
}
