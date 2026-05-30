# RK3588 AI网关项目详细实现笔记

## 1. 图像格式转换流程

### 1.1 拉流阶段的格式转换
在`rtsp_mpp_decoder.cpp`中，图像格式经历了以下转换过程：

```cpp
// 从摄像头设备(/dev/video74)或RTSP流获取原始数据
// 经过MPP解码器解码为YUV420格式
// 使用RGA转换为RGB24格式用于后续处理
```

具体实现在`decodeLoop`函数中：
- 输入：H264/H265编码的视频流
- 解码后：NV12或YUV420格式（由MPP解码器输出）
- RGA转换后：RGB24格式（用于OpenCV处理）

### 1.2 RGA格式转换细节
在`rga_yuv_converter.cpp`的`convertYUVToRGB`函数中：
- 输入格式：NV12 (YUV 4:2:0 semi-planar)
- 输出格式：RGB24 (8-bit per channel RGB)
- 使用RGA硬件加速进行格式转换，避免CPU参与

### 1.3 推流阶段的格式转换
在`streamer_thread.cpp`中：
- 拼接后的图像为RGB24格式 (1280x960)
- 通过GStreamer管道转换为NV12格式
- 使用mpph264enc进行硬件编码
- 最终输出H264编码的RTSP流

## 2. 线程安全队列机制

### 2.1 ThreadSafeQueue模板类
定义在`include/ThreadSafeQueue.h`中：
```cpp
template<typename T>
class ThreadSafeQueue {
private:
    mutable std::mutex mut;           // 保护队列的互斥锁
    std::queue<T> data_queue;         // 底层数据队列
    std::condition_variable data_cond; // 条件变量用于等待数据
};
```

### 2.2 全局队列实例
在`include/app_context.h`中定义了多个全局队列：
```cpp
extern std::array<ThreadSafeQueue<VideoFrame>, NUM_STREAMS> g_pull_queues;  // 拉流队列
extern std::array<ThreadSafeQueue<VideoFrame>, NUM_STREAMS> g_push_queues;  // 推流队列
extern std::array<ThreadSafeQueue<cv::Mat>, NUM_STREAMS> g_inference_queues; // 推理队列
```

### 2.3 队列在各线程中的作用
- **拉流线程** (`puller_thread.cpp`)：将解码后的`VideoFrame`放入`g_pull_queues[i]`
- **推理线程** (`inference_thread.cpp`)：从`g_pull_queues[i]`取出帧，处理后放入`g_inference_queues[i]`
- **推流线程** (`streamer_thread.cpp`)：从`g_push_queues[i]`获取帧进行拼接和推流

### 2.4 队列操作示例
在`updateCanvasFromPushQueues`函数中：
```cpp
while (g_push_queues[i].size() > 1) {
    VideoFrame dummy;
    g_push_queues[i].pop(dummy);  // 清除旧帧，只保留最新帧
}
if (g_push_queues[i].size() > 0 && g_push_queues[i].pop(frame)) {
    latest_frames[i] = frame;     // 获取最新帧
}
```

## 3. 关键全局变量和数据结构

### 3.1 VideoFrame结构
定义在`include/common.h`中：
```cpp
struct VideoFrame {
    cv::Mat image;              // 图像数据
    int64_t timestamp_us;       // 时间戳（微秒）
    int stream_id;              // 流ID
};
```

### 3.2 检测结果存储
在`include/app_context.h`中：
```cpp
extern std::array<std::mutex, NUM_STREAMS> g_results_mutex;     // 每个流的结果互斥锁
extern std::array<std::vector<DetectResult>, NUM_STREAMS> g_latest_results; // 每个流的最新检测结果
```

### 3.3 DetectResult结构
```cpp
struct DetectResult {
    cv::Rect box;               // 检测框
    float confidence;           // 置信度
    int class_id;               // 类别ID
};
```

## 4. 关键函数和类的作用

### 4.1 composeGridWithDetections函数
位于`src/stitcher.cpp`中，负责将4路视频流拼接成单个画面：
- 从`g_latest_results[i]`获取检测结果（使用try_lock避免阻塞）
- 将检测框绘制到对应区域
- 在右下角添加时间戳

### 4.2 decodeLoop函数
位于`src/rtsp_mpp_decoder.cpp`中，是解码的核心循环：
- 从RTSP流读取编码帧
- 使用MPP解码器解码
- 通过RGA转换格式
- 存入`g_pull_queues[i]`

### 4.3 inferenceLoop函数
位于`src/inference_thread.cpp`中，执行AI推理：
- 从`g_pull_queues[i]`获取待处理帧
- 调用RKNN模型进行推理
- 将检测结果存储到`g_latest_results[i]`（使用`g_results_mutex[i]`保护）

### 4.4 推理线程的后处理过程
推理线程的后处理主要在`inferenceLoop`函数中实现，具体步骤如下：
1. **获取输入帧**：从`g_pull_queues[i]`获取待处理的视频帧
2. **图像预处理**：调整图像尺寸至模型输入要求（通常是640x640），进行归一化处理
3. **模型推理**：调用RKNN运行时执行人脸检测推理
4. **结果解析**：解析模型输出，提取检测框坐标、置信度等信息
5. **结果过滤**：根据置信度阈值过滤检测结果
6. **坐标转换**：将检测框坐标从模型输入尺寸转换回原始图像尺寸
7. **结果存储**：将处理后的检测结果（DetectResult结构）存储到`g_latest_results[i]`
8. **线程同步**：使用`g_results_mutex[i]`保护共享结果数据的访问

### 4.5 streamerThread函数
位于`src/streamer_thread.cpp`中，负责视频推流：
- 调用`updateCanvasFromPushQueues`获取最新帧
- 调用`composeGridWithDetections`拼接画面
- 通过GStreamer管道推流

## 5. 线程同步机制

### 5.1 推理结果同步
在`stitcher.cpp`的`composeGridWithDetections`函数中：
```cpp
if (g_results_mutex[i].try_lock()) {
    current_res = g_latest_results[i];
    g_results_mutex[i].unlock();
} // 如果无法立即获得锁，则使用空的current_res
```
这种非阻塞方式确保视频帧优先展示，检测框可以延迟更新。

### 5.2 系统运行状态控制
全局变量`g_system_running`（定义在`include/app_context.h`）：
- 控制所有线程的运行状态
- 在`main.cpp`中通过信号处理器设置为false以安全退出

## 6. 硬件资源管理

### 6.1 RGA调度
通过环境变量`RK_RGA_SCHEDULER`控制RGA核心分配：
- -1: 自动分配
- 0: 固定使用RGA3_CORE0
- 1: 固定使用RGA3_CORE1
- 2: RGA3双核模式

### 6.2 NPU资源分配
在`inference_thread.cpp`中，两个推理线程分别处理不同的流：
- 推理线程1: 处理流0和流1
- 推理线程2: 处理流2和流3
- 通过RKNN上下文绑定到不同的NPU核心

## 7. 性能优化细节

### 7.1 队列大小控制
在`updateCanvasFromPushQueues`中，通过以下代码控制队列大小：
```cpp
while (g_push_queues[i].size() > 1) {  // 只保留最新帧
    VideoFrame dummy;
    g_push_queues[i].pop(dummy);
}
```

### 7.2 帧率控制
在`streamer_thread.cpp`中，使用动态延迟控制帧率：
```cpp
auto sleep_duration = std::chrono::milliseconds(33) - (std::chrono::steady_clock::now() - t3_write);
if (sleep_duration.count() > 0) {
    std::this_thread::sleep_for(sleep_duration);
}
```
33ms对应约30fps的帧间隔。

## 8. 数据流向详细说明

### 8.1 从视频源到显示的完整路径
1. **视频源** → `/dev/video74` 或 `rtsp://...`
2. **拉流线程** → `decodeLoop()` → 解码为YUV格式
3. **RGA转换** → `convertYUVToRGB()` → 转换为RGB格式
4. **存入队列** → `g_pull_queues[i].push(frame)` 
5. **推理线程** → 从`g_pull_queues[i]`取出 → RKNN推理 → 结果存入`g_latest_results[i]`
6. **推流线程** → `updateCanvasFromPushQueues()` → 从`g_push_queues[i]`获取最新帧
7. **拼接处理** → `composeGridWithDetections()` → 绘制检测框 → GStreamer推流

### 8.2 检测结果的异步更新
- 推理线程持续更新`g_latest_results[i]`和对应的`g_results_mutex[i]`
- 拼接线程使用`try_lock`非阻塞获取检测结果
- 即使没有新的检测结果，也会显示最新的视频帧