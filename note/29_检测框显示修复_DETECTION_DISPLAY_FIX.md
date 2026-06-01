# 检测框显示修复详细文档

**文档创建时间：** 2026-05-30 22:30:00
**修改时间范围：** 2026-05-30 22:25:00 - 2026-05-30 22:30:00
**修改人：** Claude Code
**修改目的：** 修复检测框不显示问题，添加调试日志

---

## 一、问题描述

### 1.1 现象
- 视频监控正常显示
- 推理线程正常工作
- 但画面上没有人脸检测框

### 1.2 根本原因

**问题1：检测框被覆盖**
```
updateFrame() 每帧覆盖ROI区域 → 检测框被擦掉
推理每45ms才完成一次 → 检测框画上去后立刻被下一帧覆盖
```

**问题2：推理结果丢失**
```
推理队列为空时 → 没有推理结果 → 检测框消失
```

---

## 二、解决方案

### 2.1 修复检测框被覆盖

**修改前：**
```cpp
void RealtimeComposer::updateFrame(int stream_id, const cv::Mat& frame) {
    // 更新帧
    cv::resize(frame, roi, rois_[stream_id].size());
    // 检测框被覆盖！
}
```

**修改后：**
```cpp
void RealtimeComposer::updateFrame(int stream_id, const cv::Mat& frame) {
    // 更新帧
    cv::resize(frame, roi, rois_[stream_id].size());
    
    // 帧更新后，重新绘制该流的检测结果（防止检测框被覆盖）
    std::vector<DetectResult> results;
    {
        std::lock_guard<std::mutex> det_lock(detection_mutexes_[stream_id]);
        results = detections_[stream_id];
    }
    if (!results.empty()) {
        drawDetections(grid_, stream_id, results);
    }
}
```

### 2.2 沿用上次推理结果

**修改前：**
```cpp
if (g_inference_queues[current_stream_id].pop(frame)) {
    // 正常推理
    std::vector<DetectResult> results = detector.inference(*frame.image);
    // ...
}
// 推理队列为空时，没有处理
```

**修改后：**
```cpp
if (g_inference_queues[current_stream_id].pop(frame)) {
    // 正常推理
    std::vector<DetectResult> results = detector.inference(*frame.image);
    
    // 保存最近一次推理结果
    g_last_inference_results[current_stream_id] = results;
    // ...
} else {
    // 推理队列为空，沿用上次的推理结果
    std::vector<DetectResult> last_results;
    {
        std::lock_guard<std::mutex> lock(g_last_results_mutex[current_stream_id]);
        last_results = g_last_inference_results[current_stream_id];
    }
    
    // 如果有历史结果，更新到合成器
    if (!last_results.empty() && g_realtime_composer) {
        g_realtime_composer->updateDetectionResults(current_stream_id, last_results);
    }
}
```

### 2.3 添加调试日志

```cpp
// DEBUG: 每30帧打印一次检测结果
if (g_inference_frame_count[current_stream_id] % 30 == 0) {
    std::cout << "[推理DEBUG] 流" << current_stream_id
              << " 第" << g_inference_frame_count[current_stream_id] << "次推理"
              << " 检测到" << results.size() << "个目标";
    for (size_t i = 0; i < results.size() && i < 3; i++) {
        std::cout << " [class:" << results[i].classId
                  << " conf:" << results[i].confidence
                  << " box:(" << results[i].box.x << "," << results[i].box.y
                  << "," << results[i].box.width << "," << results[i].box.height << ")]";
    }
    std::cout << std::endl;
}
```

---

## 三、数据流

### 3.1 修改前

```
拉流线程 → updateFrame() → 覆盖ROI → 检测框被擦掉
推理线程 → updateDetectionResults() → 画检测框 → 下一帧覆盖
```

### 3.2 修改后

```
拉流线程 → updateFrame() → 覆盖ROI → 重新绘制检测框
推理线程 → updateDetectionResults() → 保存检测结果
推理队列为空 → 沿用上次检测结果
```

---

## 四、修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `src/realtime_composer.cpp` | 帧更新后重新绘制检测框 |
| `src/inference_thread.cpp` | 沿用上次推理结果，添加调试日志 |

---

## 五、测试验证

### 5.1 编译运行

```bash
rm -rf build && mkdir build && cd build
cmake ..
make -j$(nproc)
./RK3588_AI_Gateway
```

### 5.2 预期日志

```
[推理DEBUG] 流3 第30次推理 检测到2个目标 [class:0 conf:0.85 box:(100,50,200,150)] [class:0 conf:0.72 box:(400,100,180,140)]
```

### 5.3 预期效果

- ✅ 视频监控正常显示
- ✅ 人脸检测框持续显示
- ✅ 检测框位置准确
- ✅ 推理结果沿用

---

**文档生成工具：** Claude Code
**文档版本：** v1.0
**下次更新：** 待定
