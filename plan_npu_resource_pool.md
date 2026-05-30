# NPU资源池实现计划

## 一、当前架构分析

### 1.1 现有问题
- **固定绑定**：NPU核心0处理流0、1，NPU核心1处理流2、3
- **负载不均衡**：无法根据实际负载动态分配
- **资源浪费**：3个NPU核心只用了2个
- **扩展性差**：增加流或NPU核心需要修改代码

### 1.2 当前流程
```
流0 → 队列0 → NPU核心0 → 结果0
流1 → 队列1 → NPU核心0 → 结果1
流2 → 队列2 → NPU核心1 → 结果2
流3 → 队列3 → NPU核心1 → 结果3
```

## 二、目标架构设计

### 2.1 NPU资源池架构（公平调度版）
```
流0 → 队列0 ─┐
流1 → 队列1 ─┤                ┌→ NPU核心0 (工作线程0)
流2 → 队列2 ─┼→ 流独立队列 ──┼→ NPU核心1 (工作线程1)
流3 → 队列3 ─┘                └→ NPU核心2 (工作线程2)
                             轮询调度，避免饥饿
```

### 2.2 核心组件
1. **NpuTask结构体**：封装推理任务
2. **流独立队列**：每个流有独立的任务队列，避免资源竞争
3. **NPU工作线程池**：3个线程，轮询所有流的队列
4. **公平调度算法**：Round-Robin轮询，确保每个流都能获得推理资源
5. **结果分发机制**：将推理结果分发到对应流的结果队列

## 三、详细实现方案

### 3.1 数据结构定义

```cpp
// NPU推理任务
struct NpuTask {
    int stream_id;                      // 来源流ID
    uint64_t frame_id;                  // 帧序号
    std::shared_ptr<cv::Mat> image;     // 图像数据（零拷贝）
};

// NPU工作线程上下文
struct NpuWorkerContext {
    int npu_core_index;                 // NPU核心索引
    ThreadSafeQueue<NpuTask>* task_queue; // 任务队列指针
    std::atomic<bool>* running;         // 运行标志
};
```

### 3.2 全局变量修改

```cpp
// 新增：全局NPU任务队列
extern ThreadSafeQueue<NpuTask> g_npu_task_queue;

// 修改：移除原有的g_inference_queues（可选，保留兼容性）
```

### 3.3 NPU工作线程函数

```cpp
void npuWorkerThread(const std::string& model_path,
                     int npu_core_index,
                     ThreadSafeQueue<NpuTask>& task_queue) {
    std::cout << "[NPU工作线程 " << npu_core_index << "] 启动" << std::endl;

    RKNNDetector detector;
    if (!detector.init(model_path, npu_core_index)) {
        std::cerr << "[错误] NPU核心 " << npu_core_index << " 初始化失败" << std::endl;
        return;
    }

    NpuTask task;
    while (g_system_running) {
        // 从任务队列获取任务（阻塞等待）
        if (!task_queue.pop(task)) {
            continue;  // 系统退出或队列为空
        }

        // 执行推理
        auto results = detector.inference(*task.image);

        // 将结果存入对应流的结果队列
        {
            std::lock_guard<std::mutex> lock(g_results_mutex[task.stream_id]);
            g_latest_results[task.stream_id] = std::move(results);
        }
    }

    std::cout << "[NPU工作线程 " << npu_core_index << "] 退出" << std::endl;
}
```

### 3.4 任务分发线程（替代原有推理线程）

```cpp
void inferenceDispatcherThread(int stream_id) {
    std::cout << "[推理分发线程 流" << stream_id << "] 启动" << std::endl;

    while (g_system_running) {
        VideoFrame frame;

        // 从流队列获取帧
        if (!g_inference_queues_stream[stream_id].pop(frame)) {
            continue;
        }

        // 创建NPU任务
        NpuTask task;
        task.stream_id = stream_id;
        task.frame_id = frame.frame_id;
        task.image = frame.image;  // 共享指针，零拷贝

        // 提交到全局任务队列
        g_npu_task_queue.push(std::move(task));
    }

    std::cout << "[推理分发线程 流" << stream_id << "] 退出" << std::endl;
}
```

### 3.5 修改app_lifecycle.cpp

```cpp
void startWorkerThreads(const std::vector<std::string>& stream_sources,
                        const std::string& model_path,
                        const RuntimeOptions& options,
                        WorkerThreads& workers) {
    // 1. 启动拉流线程（不变）
    workers.pullers.clear();
    workers.pullers.reserve(NUM_STREAMS);
    for (int i = 0; i < NUM_STREAMS; ++i) {
        workers.pullers.emplace_back(streamPullerAndDecoderThread, i, stream_sources[i]);
    }

    // 2. 启动NPU工作线程池（3个核心）
    workers.npu_workers.reserve(3);
    for (int i = 0; i < 3; ++i) {
        workers.npu_workers.emplace_back(npuWorkerThread, model_path, i, std::ref(g_npu_task_queue));
    }

    // 3. 启动推理分发线程（每个流一个）
    workers.dispatchers.reserve(NUM_STREAMS);
    for (int i = 0; i < NUM_STREAMS; ++i) {
        workers.dispatchers.emplace_back(inferenceDispatcherThread, i);
    }

    // 4. 启动推流线程（不变）
    workers.streamer = std::thread(streamerThread);
}
```

## 四、关键优化点

### 4.1 任务队列优化
- **队列深度**：设置合理的队列深度（如30帧）
- **丢弃策略**：队列满时丢弃最旧任务
- **优先级**：可选实现优先级队列

### 4.2 负载均衡
- **工作窃取**：空闲核心从忙碌核心的队列窃取任务
- **亲和性**：相同流的任务尽量由同一核心处理（缓存友好）
- **动态调整**：根据核心负载动态调整任务分配

### 4.3 性能监控
```cpp
struct NpuStats {
    std::atomic<uint64_t> tasks_processed{0};
    std::atomic<uint64_t> tasks_dropped{0};
    std::atomic<uint64_t> total_latency_us{0};
};
```

## 五、实现步骤

### 步骤1：修改头文件 ✅
1. 修改 `app_context.h`：添加npu_pool.h引用 ✅
2. 修改 `app_lifecycle.h`：更新WorkerThreads结构，添加NPU工作线程和分发线程 ✅
3. 创建 `npu_pool.h`：定义NpuTask、NpuWorkerStats和相关结构 ✅

### 步骤2：实现NPU工作线程 ✅
1. 创建 `npu_pool.cpp`：实现npuWorkerThread和inferenceDispatcherThread ✅
2. 实现任务队列管理和负载均衡 ✅

### 步骤3：修改推理流程 ✅
1. 修改 `inference_thread.cpp`：保留原有函数，添加注释说明废弃 ✅
2. 更新 `app_lifecycle.cpp`：创建新的线程池，实现NPU资源池 ✅
3. 修改 `ThreadSafeQueue.h`：push函数返回bool值 ✅
4. 更新 `CMakeLists.txt`：添加npu_pool.cpp到编译列表 ✅

### 步骤4：测试和优化
1. 单元测试：验证任务分发和结果正确性
2. 性能测试：对比优化前后的延迟和吞吐量
3. 压力测试：高并发场景下的稳定性

## 六、预期效果

### 6.1 性能提升
- **延迟降低**：任务立即分配给空闲核心，减少等待时间
- **吞吐量提升**：3个核心并行处理，理论提升50%
- **负载均衡**：自动适应不同流的负载差异

### 6.2 资源利用
- **NPU利用率**：从66%提升到接近100%
- **CPU占用**：减少轮询，降低CPU占用
- **内存效率**：零拷贝设计，减少内存分配

### 6.3 扩展性
- **易于扩展**：增加流或NPU核心只需调整参数
- **灵活配置**：支持动态调整工作线程数量
- **监控友好**：内置性能统计，便于调优

## 七、风险和注意事项

### 7.1 潜在风险
1. **竞态条件**：多线程访问共享数据需要仔细同步
2. **任务延迟**：队列深度设置不当可能导致延迟增加
3. **NPU冲突**：多线程同时访问同一NPU核心可能冲突

### 7.2 解决方案
1. **线程安全**：使用现有的ThreadSafeQueue，确保同步正确
2. **队列调优**：通过实验确定最佳队列深度
3. **核心隔离**：确保每个工作线程绑定到独立的NPU核心

## 八、代码变更清单

### 8.1 新增文件
- `include/npu_pool.h`
- `src/npu_pool.cpp`

### 8.2 修改文件
- `include/app_context.h`
- `include/app_lifecycle.h`
- `src/app_lifecycle.cpp`
- `src/inference_thread.cpp`
- `CMakeLists.txt`

### 8.3 保留文件
- `include/ThreadSafeQueue.h`（复用）
- `include/rknn_detector.h`（复用）

---

**计划制定时间：** 2026-05-30 15:35:00
**预计实现时间：** 2-3小时
**测试验证时间：** 1-2小时
