# 检测框坐标映射修复详细文档

**文档创建时间：** 2026-05-30 22:15:00
**修改时间范围：** 2026-05-30 22:10:00 - 2026-05-30 22:15:00
**修改人：** Claude Code
**修改目的：** 修复检测框不显示问题

---

## 一、问题描述

### 1.1 现象
- 视频监控正常显示
- 推理线程正常工作
- 但画面上没有人脸检测框

### 1.2 日志
```
[四路流-推流线程] 已推送第 1260 帧。 NV12转换耗时: 2 ms，编码推流耗时: 3 ms，RGA=成功，编码器=MPP H265
```

---

## 二、根本原因

### 2.1 坐标系统不匹配

**推理输出的坐标：**
```cpp
// rknn_detector.cpp:357-361
int left   = (int)((cx - w / 2) * scale_x);  // 像素坐标 (0~640)
int top    = (int)((cy - h / 2) * scale_y);  // 像素坐标 (0~480)
int width  = (int)(w * scale_x);
int height = (int)(h * scale_y);
res.box = cv::Rect(left, top, width, height);
```

**实时合成器的绘制逻辑（错误）：**
```cpp
// realtime_composer.cpp:71-74
int x1 = static_cast<int>(det.box.x * rois_[stream_id].width);   // 错误！
int y1 = static_cast<int>(det.box.y * rois_[stream_id].height);  // 错误！
```

### 2.2 问题分析

| 坐标类型 | 范围 | 说明 |
|---------|------|------|
| 归一化坐标 | 0~1 | 需要乘以画面尺寸 |
| 像素坐标 | 0~640 | 直接使用 |

**推理输出是像素坐标（0~640），但绘制代码假设是归一化坐标（0~1）**

**结果：**
```
检测框像素坐标：x=100, y=50, w=200, h=150
错误映射后：x=100*640=64000, y=50*480=24000
→ 坐标溢出画面，检测框不可见
```

---

## 三、解决方案

### 3.1 修改drawDetections函数

**修改前（错误）：**
```cpp
void RealtimeComposer::drawDetections(cv::Mat& grid, int stream_id, 
                                       const std::vector<DetectResult>& results) {
    cv::Mat roi = grid(rois_[stream_id]);

    for (const auto& det : results) {
        // 错误：假设det.box是归一化坐标
        int x1 = static_cast<int>(det.box.x * rois_[stream_id].width);
        int y1 = static_cast<int>(det.box.y * rois_[stream_id].height);
        int x2 = x1 + static_cast<int>(det.box.width * rois_[stream_id].width);
        int y2 = y1 + static_cast<int>(det.box.height * rois_[stream_id].height);
        // ...
    }
}
```

**修改后（正确）：**
```cpp
void RealtimeComposer::drawDetections(cv::Mat& grid, int stream_id, 
                                       const std::vector<DetectResult>& results) {
    cv::Mat roi = grid(rois_[stream_id]);

    // 原始图像尺寸（推理输入是640x480）
    const float src_width = 640.0f;
    const float src_height = 480.0f;

    // 计算缩放比例：原始图像 → ROI区域
    float scale_x = static_cast<float>(rois_[stream_id].width) / src_width;
    float scale_y = static_cast<float>(rois_[stream_id].height) / src_height;

    for (const auto& det : results) {
        // 正确：将像素坐标映射到ROI区域
        int x1 = static_cast<int>(det.box.x * scale_x);
        int y1 = static_cast<int>(det.box.y * scale_y);
        int x2 = x1 + static_cast<int>(det.box.width * scale_x);
        int y2 = y1 + static_cast<int>(det.box.height * scale_y);

        // 边界检查
        x1 = std::max(0, std::min(x1, rois_[stream_id].width - 1));
        y1 = std::max(0, std::min(y1, rois_[stream_id].height - 1));
        x2 = std::max(0, std::min(x2, rois_[stream_id].width - 1));
        y2 = std::max(0, std::min(y2, rois_[stream_id].height - 1));
        // ...
    }
}
```

---

## 四、坐标映射逻辑

### 4.1 映射流程

```
原始图像 (640x480)
    ↓ 推理输出像素坐标
检测框 (x=100, y=50, w=200, h=150)
    ↓ 计算缩放比例
scale_x = 640 / 640 = 1.0
scale_y = 480 / 480 = 1.0
    ↓ 映射到ROI区域
ROI坐标 (x=100, y=50, w=200, h=150)
    ↓ 绘制
四宫格画面
```

### 4.2 缩放比例计算

```cpp
float scale_x = static_cast<float>(rois_[stream_id].width) / 640.0f;
float scale_y = static_cast<float>(rois_[stream_id].height) / 480.0f;
```

**示例：**
- ROI区域：640x480
- 原始图像：640x480
- 缩放比例：1.0（无缩放）

---

## 五、边界检查

### 5.1 添加边界检查

```cpp
// 边界检查
x1 = std::max(0, std::min(x1, rois_[stream_id].width - 1));
y1 = std::max(0, std::min(y1, rois_[stream_id].height - 1));
x2 = std::max(0, std::min(x2, rois_[stream_id].width - 1));
y2 = std::max(0, std::min(y2, rois_[stream_id].height - 1));
```

**作用：**
- 防止坐标溢出画面
- 防止负坐标
- 确保检测框在可见区域内

---

## 六、修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `src/realtime_composer.cpp` | 修复drawDetections函数的坐标映射逻辑 |

---

## 七、测试验证

### 7.1 编译运行

```bash
rm -rf build && mkdir build && cd build
cmake ..
make -j$(nproc)
./RK3588_AI_Gateway
```

### 7.2 预期效果

- ✅ 视频监控正常显示
- ✅ 人脸检测框正确绘制
- ✅ 检测框位置准确

---

**文档生成工具：** Claude Code
**文档版本：** v1.0
**下次更新：** 待定
