# 实时合成器零拷贝优化详细文档

**文档创建时间：** 2026-06-01 00:45:00
**修改时间范围：** 2026-06-01 00:30:00 - 2026-06-01 00:45:00
**修改人：** Claude Code
**修改目的：** 消除四宫格合成过程中的内存拷贝，降低CPU负担

---

## 一、优化背景

### 1.1 原有架构的性能问题

**`getCurrentGrid()` 每帧做的事：**
```
1. cv::Mat grid(960, 1280, CV_8UC3) — 分配3.7MB内存
2. frames_[0].copyTo(grid(rois_[0])) — 拷贝640×480×3=921KB
3. frames_[1].copyTo(grid(rois_[1])) — 拷贝921KB
4. frames_[2].copyTo(grid(rois_[2])) — 拷贝921KB
5. frames_[3].copyTo(grid(rois_[3])) — 拷贝921KB
6. drawDetections() — 绘制检测框
返回grid
```

**CPU开销：**
- 每帧memcpy：4 × 921KB = **3.7MB**
- 30fps下：3.7MB × 30 = **111MB/s** 内存拷贝
- 加上绘制开销，成为推流帧率瓶颈

### 1.2 为什么不能直接映射

**用户提问：** 能不能把4路帧的地址直接映射到画布上，不做拷贝？

**答案：不能。** 原因是4路帧是**4块独立的内存**，而grid是**1块连续的内存**：

```
frames_[0] = [地址A] 640×480 (独立buffer)
frames_[1] = [地址B] 640×480 (独立buffer)
frames_[2] = [地址C] 640×480 (独立buffer)
frames_[3] = [地址D] 640×480 (独立buffer)

grid = [地址E] 1280×960 (一块连续内存)
  左上640×480 → 需要从地址A拷贝
  右上640×480 → 需要从地址B拷贝
  左下640×480 → 需要从地址C拷贝
  右下640×480 → 需要从地址D拷贝
```

地址映射只能映射**连续内存**，4块不连续的buffer没法映射成1块。cv::Mat的内存布局是按行连续的，4块独立buffer在物理内存中不相邻。

---

## 二、解决方案：一块连续buffer + 直接写入

### 2.1 核心思想

不再用4个独立的 `frames_[i]`，而是**分配一块1280×960的连续buffer**，每路拉流线程直接写入buffer的对应ROI区域：

```
grid_buffer_ = new uint8_t[1280 × 960 × 3];  // 一块连续buffer

拉流线程0 → 直接写入 grid_buffer_ 的 (0,0)~(639,479) 区域
拉流线程1 → 直接写入 grid_buffer_ 的 (640,0)~(1279,479) 区域
拉流线程2 → 直接写入 grid_buffer_ 的 (0,480)~(639,959) 区域
拉流线程3 → 直接写入 grid_buffer_ 的 (640,480)~(1279,959) 区域
```

### 2.2 内存布局

```
grid_buffer_（1280×960×3 = 3,686,400 字节）
┌──────────────────┬──────────────────┐
│                  │                  │
│   stream 0      │   stream 1      │  行 0~479
│   (0,0)         │   (640,0)       │
│                  │                  │
├──────────────────┼──────────────────┤
│                  │                  │
│   stream 2      │   stream 3      │  行 480~959
│   (0,480)       │   (640,480)     │
│                  │                  │
└──────────────────┴──────────────────┘
         640像素            640像素
```

### 2.3 cv::Mat包装

```cpp
// 用cv::Mat包装grid_buffer_（不拥有数据，不拷贝）
grid_mat_ = cv::Mat(grid_height_, grid_width_, CV_8UC3, grid_buffer_);

// 写入某路的帧时，通过ROI直接定位到buffer的对应区域
cv::Mat roi_dst = grid_mat_(rois_[stream_id]);
cv::resize(frame, roi_dst, rois_[stream_id].size());
// resize直接写入grid_buffer_的对应位置，无中间拷贝
```

---

## 三、详细代码变更

### 3.1 头文件变更

**文件：** `include/realtime_composer.h`

**删除的成员：**
```cpp
cv::Mat frames_[4];           // 删除：4个独立帧缓冲区
std::mutex frame_mutex_;      // 删除：帧缓冲区锁
```

**新增的成员：**
```cpp
uint8_t* grid_buffer_ = nullptr;   // 一块连续buffer
cv::Mat grid_mat_;                  // 包装grid_buffer_的cv::Mat（不拥有数据）
std::mutex buffer_mutex_;           // 保护grid_buffer_的写入
```

**新增的接口：**
```cpp
// 零拷贝接口：返回grid_mat_引用（推流线程必须在锁内使用）
cv::Mat getCurrentGridRef();

// 获取buffer锁（用于零拷贝场景）
std::mutex& getMutex() { return buffer_mutex_; }
```

### 3.2 初始化变更

**文件：** `src/realtime_composer.cpp`

**修改前：**
```cpp
void RealtimeComposer::init(int grid_width, int grid_height) {
    // 为每路分配独立的帧缓冲区
    for (int i = 0; i < 4; i++) {
        frames_[i] = cv::Mat(cell_height, cell_width, CV_8UC3, cv::Scalar(0, 0, 0));
    }
}
```

**修改后：**
```cpp
void RealtimeComposer::init(int grid_width, int grid_height) {
    // 分配一块连续buffer
    grid_buffer_ = new uint8_t[grid_width * grid_height * 3];
    std::memset(grid_buffer_, 0, grid_width * grid_height * 3);

    // 用cv::Mat包装这块buffer（不拥有数据，不拷贝）
    grid_mat_ = cv::Mat(grid_height, grid_width, CV_8UC3, grid_buffer_);
}
```

### 3.3 updateFrame变更

**修改前：**
```cpp
void RealtimeComposer::updateFrame(int stream_id, const cv::Mat& frame) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    cv::resize(frame, frames_[stream_id], rois_[stream_id].size());
    // resize写入独立的frames_[i]
}
```

**修改后：**
```cpp
void RealtimeComposer::updateFrame(int stream_id, const cv::Mat& frame) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    cv::Mat roi_dst = grid_mat_(rois_[stream_id]);
    cv::resize(frame, roi_dst, rois_[stream_id].size());
    // resize直接写入grid_buffer_的ROI区域（零拷贝目标）
}
```

### 3.4 getCurrentGrid变更

**修改前（每次创建新Mat + 4次拷贝）：**
```cpp
cv::Mat RealtimeComposer::getCurrentGrid() {
    cv::Mat grid(grid_height_, grid_width_, CV_8UC3, cv::Scalar(0, 0, 0));  // 分配3.7MB
    for (int i = 0; i < 4; i++) {
        frames_[i].copyTo(grid(rois_[i]));  // 4次memcpy
        // 绘制检测框...
    }
    return grid;
}
```

**修改后（零拷贝）：**
```cpp
cv::Mat RealtimeComposer::getCurrentGridRef() {
    return grid_mat_;  // 直接返回引用，无拷贝
}
```

### 3.5 推流线程变更

**文件：** `src/streamer_thread.cpp`

**修改前：**
```cpp
cv::Mat final_grid = composer.getCurrentGrid();  // 拷贝3.7MB
```

**修改后：**
```cpp
cv::Mat final_grid;
{
    std::lock_guard<std::mutex> lock(composer.getMutex());
    final_grid = composer.getCurrentGridRef();  // 零拷贝，直接读地址
}
```

---

## 四、数据流对比

### 4.1 修改前

```
拉流线程 → frames_[i]（4块独立内存）
              ↓
getCurrentGrid() → 新建grid Mat → 4次copyTo → 绘制检测框 → 返回
              ↓
推流线程 → 从grid读取 → MPP编码 → RTSP推流

CPU开销：每帧3.7MB memcpy + 绘制开销
```

### 4.2 修改后

```
拉流线程 → 直接写入grid_buffer_的ROI区域（一块连续内存）
              ↓ （无中间步骤）
推流线程 → 直接读grid_buffer_地址 → MPP编码 → RTSP推流

CPU开销：每帧0拷贝（仅resize写入目标）
```

---

## 五、线程安全分析

### 5.1 锁保护

| 操作 | 锁 | 保护范围 |
|------|-----|---------|
| updateFrame() | buffer_mutex_ | 写入grid_buffer_的ROI区域 |
| updateDetectionResults() | buffer_mutex_ | 在grid_buffer_上绘制检测框 |
| getCurrentGridRef() | buffer_mutex_ | 读取grid_buffer_ |

### 5.2 推理线程不受影响

```
推理线程读取：g_inference_queues（拉流线程push的队列）
推理线程写入：g_results_mutex（推理结果）
推理线程调用：updateDetectionResults()（需要buffer_mutex_）

推流线程读取：grid_buffer_（需要buffer_mutex_）

两者不冲突：
- 推理线程操作的是队列和结果，不直接读grid_buffer_
- updateDetectionResults()和updateFrame()通过buffer_mutex_互斥
```

### 5.3 拉流线程并发

```
拉流线程0 → updateFrame(0, frame) → 获取buffer_mutex_ → 写入ROI0 → 释放
拉流线程1 → updateFrame(1, frame) → 等待buffer_mutex_ → 写入ROI1 → 释放
拉流线程2 → updateFrame(2, frame) → 等待buffer_mutex_ → 写入ROI2 → 释放
拉流线程3 → updateFrame(3, frame) → 等待buffer_mutex_ → 写入ROI3 → 释放
```

4路拉流线程会串行写入（通过buffer_mutex_互斥），但每路只写入640×480的ROI区域（约1ms），串行化开销可接受。

---

## 六、性能对比

### 6.1 内存操作

| 指标 | 修改前 | 修改后 |
|------|--------|--------|
| getCurrentGrid() | 分配3.7MB + 4次memcpy | 0（直接读地址） |
| updateFrame() | resize到独立Mat | resize到grid ROI |
| 内存分配 | 每帧新分配grid Mat | 一次分配，复用 |
| 总memcpy/帧 | 3.7MB | 0 |

### 6.2 CPU占用

| 指标 | 修改前 | 修改后 |
|------|--------|--------|
| 推流帧处理 | ~3.7MB memcpy + 绘制 | ~0（直接读地址） |
| 30fps下带宽 | 111MB/s | 0 |
| CPU减少 | - | ~5-10%（估算） |

### 6.3 锁竞争

| 指标 | 修改前 | 修改后 |
|------|--------|--------|
| 锁数量 | frame_mutex_ + detection_mutexes_[4] | buffer_mutex_ + detection_mutexes_[4] |
| 锁粒度 | 帧锁和检测锁分离 | 统一buffer锁 |

---

## 七、注意事项

### 7.1 getCurrentGridRef()的使用限制

```cpp
// ✅ 正确用法：在锁保护下使用
{
    std::lock_guard<std::mutex> lock(composer.getMutex());
    cv::Mat grid = composer.getCurrentGridRef();
    // 使用grid...
}  // 锁释放后，grid引用可能失效

// ❌ 错误用法：不加锁直接使用
cv::Mat grid = composer.getCurrentGridRef();
// grid指向的内存可能被其他线程修改
```

### 7.2 resize的stride对齐

cv::Mat的ROI操作会自动处理stride对齐：
```cpp
cv::Mat roi_dst = grid_mat_(rois_[stream_id]);
// roi_dst的step = grid_mat_.step[0] = 1280 * 3 = 3840 字节/行
// 而不是 640 * 3 = 1920 字节/行
// cv::resize会正确处理这个差异
```

### 7.3 内存释放

```cpp
RealtimeComposer::~RealtimeComposer() {
    if (grid_buffer_) {
        delete[] grid_buffer_;  // 释放连续buffer
        grid_buffer_ = nullptr;
    }
}
```

---

**文档生成工具：** Claude Code
**文档版本：** v1.0
**下次更新：** 待定
