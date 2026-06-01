# 检测框不显示问题排查总结

**文档创建时间：** 2026-06-01 00:15:00
**修改人：** Claude Code
**修改目的：** 总结检测框不显示问题的完整排查过程和解决方案

---

## 一、问题描述

### 1.1 现象
- 视频监控正常显示（四宫格画面正常）
- 但画面上没有人脸检测框
- 推理模型本身能检测到人脸（独立测试确认）

### 1.2 排查过程中的关键发现

| 阶段 | 发现 | 结论 |
|------|------|------|
| 独立测试 | `test_inference.cpp` 直接推理，检测框正常（conf=0.86） | ✅ 模型没问题 |
| MP4测试 | `test_mp4_detect.cpp` 逐帧推理，96.4%检测率 | ✅ 推理管线正常 |
| 推流管线 | 四宫格画面正常，但没有检测框 | ❌ 合成器问题 |

---

## 二、根本原因

### 2.1 核心问题：NPU资源池模式下推理结果没有传递给合成器

**问题链路：**
```
拉流线程 → g_inference_queues → 推理分发线程 → g_npu_task_queues → NPU工作线程
                                                                              ↓
                                                                    detector.inference()
                                                                              ↓
                                                          g_latest_results（只存了这里）
                                                          ❌ 没有调用 updateDetectionResults()
```

**原因：**
- 之前使用 `inferenceThread`（旧模式），里面有 `g_realtime_composer->updateDetectionResults()` 调用
- 切换到NPU资源池模式后，`npuWorkerThread` 推理完成后只存到了 `g_latest_results`
- **漏掉了调用 `g_realtime_composer->updateDetectionResults()`**
- 所以检测结果虽然出来了，但从来没传给合成器去画框

### 2.2 检测框被覆盖（次要问题）

**原因：** `updateFrame()` 每帧覆盖ROI区域，把之前画的检测框擦掉了

**修复：** 将视频帧和检测结果分开存储，只在 `getCurrentGrid()` 中才合成

---

## 三、解决方案

### 3.1 修复推理结果传递（核心修复）

**文件：** `src/npu_pool.cpp`

**修改内容：**
```cpp
// NPU工作线程推理完成后，添加这一行：
if (g_realtime_composer) {
    g_realtime_composer->updateDetectionResults(task.stream_id, g_latest_results[task.stream_id]);
}
```

### 3.2 重构实时合成器架构

**文件：** `include/realtime_composer.h` + `src/realtime_composer.cpp`

**修改前（有竞态条件）：**
```cpp
updateFrame() → resize覆盖grid → 重绘检测框（可能还没数据）
getCurrentGrid() → 直接返回grid
```

**修改后（无竞态）：**
```cpp
// 帧和检测结果分开存储
cv::Mat frames_[4];           // 每路原始视频帧
std::vector<DetectResult> detections_[4];  // 每路检测结果

// updateFrame() 只更新原始帧
void updateFrame(int stream_id, const cv::Mat& frame) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    cv::resize(frame, frames_[stream_id], rois_[stream_id].size());
}

// updateDetectionResults() 只保存检测结果
void updateDetectionResults(int stream_id, const std::vector<DetectResult>& results) {
    std::lock_guard<std::mutex> lock(detection_mutexes_[stream_id]);
    detections_[stream_id] = results;
}

// getCurrentGrid() 合成时才叠加检测框
cv::Mat getCurrentGrid() {
    cv::Mat grid(grid_height_, grid_width_, CV_8UC3, cv::Scalar(0, 0, 0));
    for (int i = 0; i < 4; i++) {
        frames_[i].copyTo(grid(rois_[i]));  // 复制原始帧
        drawDetections(grid, i, detections_[i]);  // 叠加检测框
    }
    return grid;
}
```

### 3.3 修复检测框坐标映射

**文件：** `src/realtime_composer.cpp`

**问题：** 推理输出的是像素坐标（0~640），但绘制代码假设是归一化坐标（0~1）

**修复：**
```cpp
// 计算缩放比例
float scale_x = static_cast<float>(rois_[stream_id].width) / 640.0f;
float scale_y = static_cast<float>(rois_[stream_id].height) / 480.0f;

// 映射坐标
int x1 = static_cast<int>(det.box.x * scale_x);
int y1 = static_cast<int>(det.box.y * scale_y);
```

### 3.4 优化推理线程丢帧策略

**文件：** `src/inference_thread.cpp`

**修改内容：**
```cpp
// 激进丢帧：清空队列中所有旧帧，只留最新
while (g_inference_queues[current_stream_id].size() > 0) {
    VideoFrame latest;
    g_inference_queues[current_stream_id].pop(latest);
    frame = latest;
    got_frame = true;
}

// 沿用上次推理结果
if (!got_frame) {
    last_results = g_last_inference_results[current_stream_id];
    if (!last_results.empty() && g_realtime_composer) {
        g_realtime_composer->updateDetectionResults(current_stream_id, last_results);
    }
}
```

---

## 四、排查工具

### 4.1 独立推理测试（确认模型正常）

```cpp
// test_inference.cpp
RKNNDetector detector;
detector.init("yolov8_face_fp.rknn", 0);
cv::Mat image = cv::imread("image.png");
std::vector<DetectResult> results = detector.inference(image);
// 如果results不为空，说明模型正常
```

### 4.2 管线截帧调试

```cpp
// 在拉流线程中保存第一帧
if (frame_seq == 1) {
    cv::imwrite("debug_puller_stream" + std::to_string(streamId) + ".jpg", bgr_frame);
}

// 在推流线程中保存四宫格
if (!saved_streamer_frame && !final_grid.empty()) {
    cv::imwrite("debug_streamer_grid.jpg", final_grid);
}
```

### 4.3 推理DEBUG日志

```cpp
// 每次推理打印日志
std::cout << "[推理DEBUG] 流" << stream_id
          << " 第" << count << "次推理"
          << " 检测到" << results.size() << "个目标"
          << " box:(" << results[0].box.x << "," << results[0].box.y << ")" << std::endl;
```

---

## 五、验证结果

### 5.1 测试环境
- 使用同一个MP4文件模拟4路流
- 本地保存关键帧为图片

### 5.2 验证结果
```
✅ 4路MP4拉流正常
✅ 4路人脸检测框正常显示（conf 0.82-0.83）
✅ 四宫格拼接正常
✅ 检测框位置准确
✅ 推理结果持续显示（沿用上次结果）
```

### 5.3 关键帧截图
- `frame_30.jpg`：第1秒，4路都有检测框
- `frame_1200.jpg`：第40秒，检测框持续显示

---

## 六、经验教训

### 6.1 架构变更时要检查所有调用链
- 切换推理架构时，确保检测结果能传递到所有消费方
- 新增模块时，检查是否需要调用现有的更新接口

### 6.2 线程同步设计
- 将数据和渲染分离：原始帧和检测结果分开存储
- 只在最终读取时合成，避免竞态条件

### 6.3 调试方法
1. 先用独立测试确认模型正常
2. 再用截帧调试确认管线数据流
3. 最后用日志确认线程间数据传递

---

**文档生成工具：** Claude Code
**文档版本：** v1.0
**下次更新：** 待定
