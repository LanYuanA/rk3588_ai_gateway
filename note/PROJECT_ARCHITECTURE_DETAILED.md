# RK3588 AI Video Gateway 项目详细架构说明

## 项目概述

本项目是一个基于RK3588芯片的多路视频AI处理与RTSP推流网关，充分利用了RK3588的硬件加速能力（NPU、RGA、VPU/MPP），实现对多路视频流的同时拉取、AI目标检测、画面拼接和硬件编码推流。

## 硬件架构组件

### 1. NPU (Neural Processing Unit)
- **功能**: AI推理加速，执行YOLOv8等深度学习模型
- **性能**: 6TOPS算力，支持多核并发推理
- **API接口**: RKNN-Toolkit2 C++ API

### 2. RGA (Raster Graphic Acceleration Unit)
- **功能**: 2D图形加速，图像缩放、格式转换、拼接
- **API接口**: librga/im2d API
- **关键函数**: `imcvtcolor`, `imresize`, `imcopy`, `improcess`

### 3. VPU (Video Processing Unit) / MPP
- **功能**: 视频编解码加速
- **API接口**: GStreamer MPP插件
- **关键组件**: `mpph264enc`

## 软件架构与线程模型

### 1. 拉流线程 (Puller Thread)
**文件**: `src/puller_thread.cpp`

#### 主要函数: `streamPullerAndDecoderThread(int streamId, const std::string& stream_url)`

**功能流程**:
1. 根据URL类型选择不同的视频源处理方式
2. 实时拉取视频帧并进行预处理
3. 将视频帧放入推理队列

**具体实现**:

```cpp
void streamPullerAndDecoderThread(int streamId, const std::string& stream_url) {
    const bool is_v4l2_stream = (stream_url.find("/dev/video") != std::string::npos);
    const bool is_rtsp_stream = (stream_url.find("rtsp://") != std::string::npos);
    const bool use_rga_resize = envFlagEnabled("RK_USE_RGA_RESIZE", true);
    const bool use_dma32_for_rga = envFlagEnabled("RK_RGA_USE_DMA32", true);
    const std::string dma_heap_path = envStringValue("RK_DMA_HEAP_PATH", "/dev/dma_heap/system-dma32");
    const int rga_scheduler_mode = envIntValue("RK_RGA_SCHEDULER", -1);
```

**硬件组件参与**:

#### V4L2摄像头处理
```cpp
V4L2Capture v4l2_cap;
if (is_v4l2_stream) {
    if (!v4l2_cap.open(stream_url, 640, 480)) {
        std::cerr << "[错误] V4L2 打开摄像头失败: " << stream_url << std::endl;
        return;
    }
    // 初始化V4L2捕获器的RGA YUV转换器上下文
    initializeRgaYuvConverterContext(streamId,
                                   true,  // use_rga_conversion
                                   use_dma32_for_rga,
                                   rga_scheduler_mode,
                                   v4l2_cap.getYuvConverterContext());
}
```

**CPU工作**:
- 打开V4L2设备文件
- 配置视频格式 (VIDIOC_S_FMT)
- 内存映射 (mmap) 和缓冲区管理
- select()等待数据就绪

**RGA工作**:
- YUV到BGR颜色空间转换 (YUYV -> BGR)
- 使用 `imcvtcolor` API进行硬件加速转换
- DMA32内存管理避免4GB寻址限制

#### RTSP流处理
```cpp
else if (stream_url.find("rtsp://") != std::string::npos) {
    setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp|stimeout;2000000|flags;low_delay|video_codec;h264", 1);
    cap.open(stream_url, cv::CAP_FFMPEG);
}
```

**CPU工作**:
- FFmpeg解码RTSP流
- 解码后的BGR帧存储

#### 本地文件处理
```cpp
else {
    std::string local_file_uri = "file://" + getProjectRootDir() + "/" + stream_url;
    std::string gst_pipeline = "uridecodebin uri=" + local_file_uri + " ! videoconvert ! appsink sync=false max-buffers=5";
    cap.open(gst_pipeline, cv::CAP_GSTREAMER);
    if (!cap.isOpened()) {
        std::cout << "[警告] 启用 GStreamer 加速播放文件失败，回退到普通读取..." << std::endl;
        cap.open(stream_url);
    }
}
```

**VPU工作**:
- GStreamer MPP解码器加速解码本地视频文件

#### 视频帧处理循环
```cpp
while (g_system_running) {
    bool ret = false;
    if (is_v4l2_stream) {
        ret = v4l2_cap.read(bgr_frame);  // V4L2读取
    } else {
        ret = cap.read(bgr_frame);       // OpenCV读取
    }
    
    if (!ret || bgr_frame.empty()) {
        // 重连逻辑
    }
    
    // 尺寸调整
    if (bgr_frame.cols != 640 || bgr_frame.rows != 480) {
        bool resized_by_rga = tryResizeTo640x480WithRga(bgr_frame, dma_heap_path, rga_context);
        if (!resized_by_rga) {
            cv::resize(bgr_frame, bgr_frame, cv::Size(640, 480));  // CPU回退
        }
    }
    
    // 发送到推理队列
    VideoFrame frame;
    frame.stream_id = streamId;
    frame.frame_id = frame_seq++;
    frame.image = bgr_frame.clone();
    
    g_inference_queues[streamId].push(frame);
    g_push_queues[streamId].push(frame);
}
```

**RGA工作**:
- 使用 `imresize` API进行硬件加速缩放
- DMA32内存管理

### 2. 推理线程 (Inference Thread)
**文件**: `src/inference_thread.cpp`

#### 主要函数: `inferenceWorker(int threadId)`

**功能流程**:
1. 从拉流队列获取视频帧
2. 在NPU上执行AI推理
3. 将检测结果发送到推流线程

**具体实现**:
```cpp
void inferenceWorker(int threadId) {
    // 初始化RKNN模型
    RKDetector detector;
    if (!detector.initModel(getProjectRootDir() + "/yolov8_face_fp.rknn")) {
        std::cerr << "[错误] NPU推理模型加载失败" << std::endl;
        return;
    }
    
    while (g_system_running) {
        // 从队列获取帧
        for (int streamId = 0; streamId < 4; ++streamId) {
            VideoFrame frame;
            if (g_inference_queues[streamId].try_pop(frame)) {
                // 执行NPU推理
                std::vector<DetectionResult> detections = detector.detect(frame.image);
                
                // 将检测结果发送到推流线程
                DetectionFrame det_frame;
                det_frame.stream_id = frame.stream_id;
                det_frame.frame_id = frame.frame_id;
                det_frame.image = frame.image.clone();
                det_frame.detections = detections;
                
                g_detection_results.push(det_frame);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 避免忙等待
    }
}
```

**NPU工作**:
- 加载YOLOv8人脸检测模型到NPU
- 执行 `rknn_run()` API进行推理
- 使用 `rknn_outputs_get()` 获取检测结果
- 多核并发推理（RK3588有3个NPU核心）

**关键NPU API**:
- `rknn_init()`: 初始化NPU推理上下文
- `rknn_inputs_set()`: 设置输入数据
- `rknn_run()`: 执行模型推理
- `rknn_outputs_get()`: 获取推理输出
- `rknn_destroy()`: 释放NPU资源

### 3. 推流线程 (Streamer Thread)
**文件**: `src/streamer_thread.cpp`

#### 主要函数: `streamerThread()`

**功能流程**:
1. 从推理结果队列获取带检测框的帧
2. 使用RGA进行画面拼接
3. 使用VPU进行H.264编码
4. 通过RTSP推流到MediaMTX

**具体实现**:
```cpp
void streamerThread() {
    // 初始化GStreamer管道
    GstElement *pipeline, *appsrc, *conv, *encoder, *rtsp_client_sink;
    GstCaps *caps;
    
    // ... GStreamer管道初始化 ...
    
    // RGA上下文初始化
    RgaResizeContext rga_context;
    initializeRgaResizeContext(0, true, true, -1, rga_context);
    
    while (g_system_running) {
        // 收集4路检测结果
        std::vector<DetectionFrame> frames;
        for (int i = 0; i < 4; ++i) {
            DetectionFrame frame;
            if (g_detection_results.try_pop(frame)) {
                frames.push_back(frame);
            }
        }
        
        if (frames.size() >= 4) {
            // 使用RGA进行2x2拼接
            cv::Mat stitched_frame = stitchFramesWithRga(frames);
            
            // 使用GStreamer进行编码和推流
            pushFrameToStream(stitched_frame, pipeline, appsrc);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
```

**RGA工作**:
- 2x2画面拼接使用 `imcopy` API
- 在1280x960画布上进行硬件加速拼接
- 使用 `improcess` 进行复合图像处理

**关键RGA API**:
- `imbeginJob()`: 开始RGA任务
- `imcopy()`: 硬件加速图像拷贝
- `imresize()`: 硬件加速图像缩放
- `improcess()`: 硬件加速图像复合处理
- `imendJob()`: 提交并执行RGA任务
- `wrapbuffer_handle()`: 封装图像缓冲区结构
- `importbuffer_T()`: 导入外部内存供RGA使用

**VPU工作**:
- 使用GStreamer MPP插件进行H.264硬件编码压缩
- `mpph264enc` 硬件编码器执行视频压缩
- RTSP推流到MediaMTX服务器

**关键VPU API (通过GStreamer)**:
- `appsrc`: 应用程序数据源
- `videoconvert`: 颜色空间转换
- `mpph264enc`: MPP硬件H.264编码器（执行视频压缩）
- `rtspclientsink`: RTSP客户端推流器

**视频压缩详细说明**:
- **压缩标准**: H.264/AVC
- **压缩方式**: 硬件压缩（利用RK3588的VPU）
- **压缩时机**: 在推流阶段，对拼接后的完整画面进行压缩
- **压缩目的**: 减少网络带宽占用，提高传输效率
- **压缩质量**: 通过GStreamer参数可调节比特率和质量
- **编码器**: Rockchip MPP (Media Processing Platform) 硬件编码器

## 详细API调用流程

### 拉流阶段 (Pull Phase)

#### V4L2摄像头拉流
```cpp
// CPU操作
open()          // 打开设备文件
ioctl()         // 配置视频格式 (VIDIOC_S_FMT)
ioctl()         // 请求缓冲区 (VIDIOC_REQBUFS)
mmap()          // 内存映射
ioctl()         // 查询缓冲区 (VIDIOC_QUERYBUF)
ioctl()         // 队列缓冲区 (VIDIOC_QBUF)
ioctl()         // 启动流 (VIDIOC_STREAMON)

// RGA操作 (YUV->BGR转换)
imcvtcolor()    // 硬件加速颜色空间转换
```

#### RGA YUV转换详细流程
```cpp
bool tryConvertYuvToBgrWithRga(void* src_buffer,
                               int width,
                               int height,
                               int bytes_per_line,
                               cv::Mat& dst_bgr_frame,
                               const std::string& dma_heap_path,
                               RgaYuvConverterContext& context) {
    // 1. 检查RGA转换是否启用
    if (!context.use_rga_conversion || context.conversion_fused_off) {
        return false;
    }

    // 2. 配置RGA调度器
    IM_STATUS cfg_runtime_ret = imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0);
    
    // 3. 确保DMA缓冲区
    bool src_ok = runtime_cache.src_dma.ensure(src_bytes, dma_heap_path);
    bool dst_ok = runtime_cache.dst_dma.ensure(dst_bytes, dma_heap_path);
    
    if (src_ok && dst_ok) {
        // 4. 复制源数据到DMA缓冲区
        std::memcpy(src_ptr, src_original, src_bytes);
        
        // 5. 创建RGA缓冲区描述符
        rga_buffer_t src_img = wrapbuffer_handle(runtime_cache.src_dma.handle(),
                                                 width, height,
                                                 RK_FORMAT_YUYV_422,
                                                 width * 2, height);
        
        rga_buffer_t dst_img = wrapbuffer_handle(runtime_cache.dst_dma.handle(),
                                                 width, height,
                                                 RK_FORMAT_BGR_888,
                                                 width * 3, height);
        
        // 6. 执行硬件转换
        IM_STATUS rga_status = imcvtcolor(src_img, dst_img, 
                                          RK_FORMAT_YUYV_422, RK_FORMAT_BGR_888);
        
        if (rga_status == IM_STATUS_SUCCESS) {
            // 7. 创建OpenCV Mat视图
            dst_bgr_frame = cv::Mat(height, width, CV_8UC3, 
                                    runtime_cache.dst_dma.addr(), width * 3);
            return true;
        }
    }
    return false;
}
```

### 推理阶段 (Inference Phase)

#### NPU推理详细流程
```cpp
std::vector<DetectionResult> RKDetector::detect(const cv::Mat& image) {
    // 1. 预处理图像
    cv::Mat input_image = preprocessImage(image);
    
    // 2. 设置输入数据
    rknn_input inputs[1];
    inputs[0].index = 0;
    inputs[0].buf = input_image.data;
    inputs[0].size = input_image.total() * input_image.elemSize();
    inputs[0].pass_through = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    
    rknn_inputs_set(ctx_, 1, inputs);
    
    // 3. 执行NPU推理
    int ret = rknn_run(ctx_, nullptr);
    if (ret < 0) {
        std::cerr << "NPU推理失败" << std::endl;
        return {};
    }
    
    // 4. 获取输出结果
    rknn_output outputs[2];
    outputs[0].want_float = 1;
    outputs[0].is_prealloc = 0;
    outputs[1].want_float = 1;
    outputs[1].is_prealloc = 0;
    
    ret = rknn_outputs_get(ctx_, 2, outputs, NULL);
    if (ret < 0) {
        std::cerr << "获取NPU输出失败" << std::endl;
        return {};
    }
    
    // 5. 后处理检测结果
    std::vector<DetectionResult> results = postProcess(outputs[0].buf, outputs[1].buf);
    
    // 6. 释放输出缓冲区
    rknn_outputs_release(ctx_, 2, outputs);
    
    return results;
}
```

### 推流阶段 (Streaming Phase)

#### RGA画面拼接详细流程
```cpp
cv::Mat stitchFramesWithRga(const std::vector<DetectionFrame>& frames) {
    // 1. 创建目标画布
    cv::Mat canvas(960, 1280, CV_8UC3);
    
    // 2. 初始化RGA任务
    IM_JOB_HANDLE job = imbeginJob();
    
    for (int i = 0; i < frames.size(); ++i) {
        // 3. 计算目标位置
        int dst_x = (i % 2) * 640;  // 每行两个画面
        int dst_y = (i / 2) * 480;  // 每列两个画面
        
        // 4. 创建源和目标缓冲区描述符
        rga_buffer_t src_img = wrapbuffer_virtualaddr(frames[i].image.data,
                                                     frames[i].image.cols,
                                                     frames[i].image.rows,
                                                     RK_FORMAT_BGR_888);
        
        rga_buffer_t dst_img = wrapbuffer_virtualaddr(canvas.data,
                                                     canvas.cols,
                                                     canvas.rows,
                                                     RK_FORMAT_BGR_888);
        
        // 5. 设置裁剪区域
        rga_rect_t src_rect = {
            .x = 0, .y = 0,
            .w = frames[i].image.cols,
            .h = frames[i].image.rows,
            .vsw = frames[i].image.cols,
            .vsh = frames[i].image.rows
        };
        
        rga_rect_t dst_rect = {
            .x = dst_x, .y = dst_y,
            .w = frames[i].image.cols,
            .h = frames[i].image.rows,
            .vsw = canvas.cols,
            .vsh = canvas.rows
        };
        
        // 6. 添加RGA拷贝任务
        IM_STATUS ret = imcopyTask(job, src_img, dst_img, &src_rect, &dst_rect);
        if (ret != IM_STATUS_SUCCESS) {
            std::cerr << "RGA拷贝任务失败" << std::endl;
            break;
        }
    }
    
    // 7. 执行RGA任务
    IM_STATUS ret = imendJob(job, NULL, 0);
    if (ret != IM_STATUS_SUCCESS) {
        std::cerr << "RGA任务执行失败" << std::endl;
    }
    
    return canvas;
}
```

#### VPU编码推流详细流程
```cpp
void pushFrameToStream(const cv::Mat& frame, GstElement* pipeline, GstElement* appsrc) {
    // 1. 准备GStreamer缓冲区
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, frame.total() * frame.elemSize(), NULL);
    
    // 2. 复制图像数据到GStreamer缓冲区
    GstMapInfo map;
    gst_buffer_map(buffer, &map, GST_MAP_WRITE);
    memcpy(map.data, frame.data, frame.total() * frame.elemSize());
    gst_buffer_unmap(buffer, &map);
    
    // 3. 设置缓冲区时间戳
    GST_BUFFER_PTS(buffer) = g_get_monotonic_time() * 1000;
    GST_BUFFER_DURATION(buffer) = 33 * GST_MSECOND;  // 30fps
    
    // 4. 推送缓冲区到GStreamer管道
    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc, "push-buffer", buffer, &ret);
    
    // 5. 释放缓冲区
    gst_buffer_unref(buffer);
    
    if (ret != GST_FLOW_OK) {
        std::cerr << "GStreamer缓冲区推送失败" << std::endl;
    }
}
```

## 硬件资源分配与优化

### CPU使用优化
- 多线程流水线架构，避免单线程瓶颈
- 无锁队列减少线程同步开销
- 智能回退机制，当硬件加速不可用时使用软件实现

### NPU使用优化
- 模型量化为INT8减少内存占用
- 多核并发推理提高吞吐量
- 异步推理避免阻塞

### RGA使用优化
- DMA32内存管理避免4GB寻址限制
- 任务批处理减少API调用开销
- 缓冲区复用减少内存分配

### VPU使用优化
- 硬件编码器直接处理原始数据
- GStreamer管道优化减少数据拷贝
- RTSP TCP模式保证传输稳定性

## 性能监控与调试

### 环境变量控制
- `RK_USE_RGA_RESIZE`: 控制是否使用RGA缩放
- `RK_RGA_USE_DMA32`: 控制是否使用DMA32内存
- `RK_RGA_SCHEDULER`: 控制RGA调度模式
- `RK_INFER_BATCH_SIZE`: 控制推理批次大小

### 熔断机制
- 当RGA转换失败时自动回退到CPU处理
- 当NPU推理失败时跳过当前帧
- 当推流失败时自动重连

这个架构充分利用了RK3588的硬件加速能力，在保持高实时性的同时显著降低了CPU负载，实现了高效的多路视频AI处理系统。