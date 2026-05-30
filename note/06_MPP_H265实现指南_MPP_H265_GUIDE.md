# MPP H265编码解码实现指南

**文档创建时间：** 2026-05-30 15:45:00
**目标：** 接入瑞芯微官方MPP接口，实现H265编码解码

---

## 一、当前架构分析

### 1.1 GStreamer直推NV12含义

**当前推流管线：**
```
appsrc (NV12原始数据) → mpph264enc → h264parse → rtspclientsink
```

**"直推NV12"含义：**
- 应用层直接推送NV12格式的原始图像数据到GStreamer管线
- GStreamer的`mpph264enc`插件内部调用MPP进行H264硬件编码
- **优点：** 简单，GStreamer封装了复杂的MPP API
- **缺点：** 灵活性差，无法精细控制编码参数

### 1.2 当前已有的MPP编码

**是的，当前已经在使用MPP编码！**
- `mpph264enc` 是GStreamer的MPP编码插件
- 它内部调用 `librockchip_mpp.so` 进行H264硬件编码
- 但用户想要更直接的控制，需要直接调用MPP API

---

## 二、MPP API 核心概念

### 2.1 MPP架构
```
应用层 (C/C++)
    ↓
MPI接口 (rk_mpi.h)
    ↓
MPP核心 (librockchip_mpp.so)
    ↓
硬件编码器 (VPU)
```

### 2.2 核心数据结构

```cpp
// 编码器上下文
MppCtx ctx;

// 编码器API
MppApi *mpi;

// 输入帧（NV12原始数据）
MppFrame frame;

// 输出包（H265编码数据）
MppPacket packet;

// 编码器配置
MppEncCfg cfg;
```

### 2.3 编码流程
```
1. mpp_create() - 创建编码器上下文
2. mpp_init() - 初始化编码器
3. 配置编码参数（分辨率、码率、帧率等）
4. 循环：
   a. 创建MppFrame，填充NV12数据
   b. 调用mpi->encode_put_frame()送入编码器
   c. 调用mpi->encode_get_packet()获取编码后的H265数据
5. mpp_destroy() - 销毁编码器
```

---

## 三、H265编码实现方案

### 3.1 头文件引用

```cpp
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_venc_cmd.h>
```

### 3.2 编码器类设计

```cpp
class MppH265Encoder {
public:
    MppH265Encoder();
    ~MppH265Encoder();

    // 初始化编码器
    bool init(int width, int height, int fps, int bitrate);

    // 编码一帧
    bool encode(const cv::Mat& nv12_frame, std::vector<uint8_t>& h265_data);

    // 释放资源
    void release();

private:
    MppCtx ctx = nullptr;
    MppApi *mpi = nullptr;
    MppEncCfg cfg = nullptr;
    int width_ = 0;
    int height_ = 0;
};
```

### 3.3 核心实现代码

```cpp
bool MppH265Encoder::init(int width, int height, int fps, int bitrate) {
    width_ = width;
    height_ = height;

    // 1. 创建编码器上下文
    MPP_RET ret = mpp_create(&ctx, &mpi);
    if (ret != MPP_OK) {
        std::cerr << "[MPP] 创建编码器失败: " << ret << std::endl;
        return false;
    }

    // 2. 初始化编码器（H265类型）
    ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC);
    if (ret != MPP_OK) {
        std::cerr << "[MPP] 初始化编码器失败: " << ret << std::endl;
        return false;
    }

    // 3. 获取默认配置
    ret = mpp_enc_cfg_init(&cfg);
    if (ret != MPP_OK) {
        std::cerr << "[MPP] 初始化配置失败: " << ret << std::endl;
        return false;
    }

    // 4. 配置编码参数
    // 分辨率
    mpp_enc_cfg_set_s32(cfg, "prep:width", width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", width);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", height);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);  // NV12

    // 码率控制
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_VBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bitrate);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bitrate * 1.5);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bitrate * 0.5);

    // 帧率
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);

    // GOP（关键帧间隔）
    mpp_enc_cfg_set_s32(cfg, "rc:gop", fps * 2);  // 2秒一个关键帧

    // 5. 应用配置
    ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    if (ret != MPP_OK) {
        std::cerr << "[MPP] 应用配置失败: " << ret << std::endl;
        return false;
    }

    std::cout << "[MPP] H265编码器初始化成功: " << width << "x" << height
              << " @" << fps << "fps, " << bitrate/1000 << "kbps" << std::endl;
    return true;
}

bool MppH265Encoder::encode(const cv::Mat& nv12_frame, std::vector<uint8_t>& h265_data) {
    if (!ctx || !mpi) return false;

    // 1. 创建MppFrame
    MppFrame frame = nullptr;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, width_);
    mpp_frame_set_height(frame, height_);
    mpp_frame_set_hor_stride(frame, width_);
    mpp_frame_set_ver_stride(frame, height_);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);

    // 2. 创建MppBuffer并复制数据
    MppBuffer buf = nullptr;
    size_t buf_size = width_ * height_ * 3 / 2;
    mpp_buffer_get(nullptr, &buf, buf_size, MPP_BUFFER_TYPE_ION);

    void *buf_ptr = mpp_buffer_get_ptr(buf);
    memcpy(buf_ptr, nv12_frame.data, buf_size);

    mpp_frame_set_buffer(frame, buf);

    // 3. 送入编码器
    MPP_RET ret = mpi->encode_put_frame(ctx, frame);
    if (ret != MPP_OK) {
        std::cerr << "[MPP] 送入编码器失败: " << ret << std::endl;
        mpp_buffer_put(buf);
        mpp_frame_deinit(&frame);
        return false;
    }

    // 4. 获取编码后的数据
    MppPacket packet = nullptr;
    ret = mpi->encode_get_packet(ctx, &packet);
    if (ret != MPP_OK || !packet) {
        std::cerr << "[MPP] 获取编码数据失败: " << ret << std::endl;
        mpp_buffer_put(buf);
        mpp_frame_deinit(&frame);
        return false;
    }

    // 5. 复制编码数据
    void *packet_data = mpp_packet_get_data(packet);
    size_t packet_size = mpp_packet_get_length(packet);
    h265_data.assign((uint8_t*)packet_data, (uint8_t*)packet_data + packet_size);

    // 6. 释放资源
    mpp_packet_deinit(&packet);
    mpp_buffer_put(buf);
    mpp_frame_deinit(&frame);

    return true;
}

void MppH265Encoder::release() {
    if (ctx) {
        mpi->reset(ctx);
        mpp_destroy(ctx);
        ctx = nullptr;
    }
    if (cfg) {
        mpp_enc_cfg_deinit(cfg);
        cfg = nullptr;
    }
}
```

---

## 四、H265解码实现方案

### 4.1 解码器类设计

```cpp
class MppH265Decoder {
public:
    MppH265Decoder();
    ~MppH265Decoder();

    // 初始化解码器
    bool init();

    // 解码一帧
    bool decode(const std::vector<uint8_t>& h265_data, cv::Mat& nv12_frame);

    // 释放资源
    void release();

private:
    MppCtx ctx = nullptr;
    MppApi *mpi = nullptr;
};
```

### 4.2 解码核心实现

```cpp
bool MppH265Decoder::init() {
    // 1. 创建解码器上下文
    MPP_RET ret = mpp_create(&ctx, &mpi);
    if (ret != MPP_OK) {
        std::cerr << "[MPP] 创建解码器失败: " << ret << std::endl;
        return false;
    }

    // 2. 初始化解码器（H265类型）
    ret = mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingHEVC);
    if (ret != MPP_OK) {
        std::cerr << "[MPP] 初始化解码器失败: " << ret << std::endl;
        return false;
    }

    std::cout << "[MPP] H265解码器初始化成功" << std::endl;
    return true;
}

bool MppH265Decoder::decode(const std::vector<uint8_t>& h265_data, cv::Mat& nv12_frame) {
    if (!ctx || !mpi) return false;

    // 1. 创建MppPacket
    MppPacket packet = nullptr;
    mpp_packet_init(&packet, h265_data.data(), h265_data.size());
    mpp_packet_set_pts(packet, 0);

    // 2. 送入解码器
    MPP_RET ret = mpi->decode_put_packet(ctx, packet);
    if (ret != MPP_OK) {
        std::cerr << "[MPP] 送入解码器失败: " << ret << std::endl;
        mpp_packet_deinit(&packet);
        return false;
    }

    // 3. 获取解码后的帧
    MppFrame frame = nullptr;
    ret = mpi->decode_get_frame(ctx, &frame);
    if (ret != MPP_OK || !frame) {
        mpp_packet_deinit(&packet);
        return false;
    }

    // 4. 检查帧是否有效
    if (!mpp_frame_get_info_change(frame)) {
        // 帧信息未变化，正常解码帧
        int width = mpp_frame_get_width(frame);
        int height = mpp_frame_get_height(frame);
        int hor_stride = mpp_frame_get_hor_stride(frame);
        int ver_stride = mpp_frame_get_ver_stride(frame);

        // 5. 获取解码数据
        MppBuffer buf = mpp_frame_get_buffer(frame);
        void *buf_ptr = mpp_buffer_get_ptr(buf);

        // 6. 复制到cv::Mat
        nv12_frame.create(ver_stride * 3 / 2, hor_stride, CV_8UC1);
        memcpy(nv12_frame.data, buf_ptr, hor_stride * ver_stride * 3 / 2);

        // 裁剪到实际尺寸
        if (hor_stride != width || ver_stride != height) {
            nv12_frame = nv12_frame(cv::Rect(0, 0, width, height)).clone();
        }
    }

    // 7. 释放资源
    mpp_frame_deinit(&frame);
    mpp_packet_deinit(&packet);

    return true;
}

void MppH265Decoder::release() {
    if (ctx) {
        mpi->reset(ctx);
        mpp_destroy(ctx);
        ctx = nullptr;
    }
}
```

---

## 五、集成到现有系统

### 5.1 修改CMakeLists.txt

```cmake
# 添加MPP库
find_library(MPP_LIB rockchip_mpp PATHS /usr/lib/aarch64-linux-gnu)
target_link_libraries(${PROJECT_NAME} PRIVATE ... ${MPP_LIB})
```

### 5.2 创建MPP封装文件

**新增文件：**
- `include/mpp_encoder.h`
- `include/mpp_decoder.h`
- `src/mpp_encoder.cpp`
- `src/mpp_decoder.cpp`

### 5.3 修改推流线程

**替换GStreamer管线为MPP直接编码：**

```cpp
// 原来的GStreamer管线
"appsrc ! mpph264enc ! h264parse ! rtspclientsink"

// 新的MPP直接编码
MppH265Encoder encoder;
encoder.init(1280, 960, 30, 4000000);  // 4Mbps

while (running) {
    cv::Mat nv12_frame = getNv12Frame();
    std::vector<uint8_t> h265_data;
    encoder.encode(nv12_frame, h265_data);
    // 发送h265_data到RTSP服务器
}
```

---

## 六、性能对比

### 6.1 当前方案（GStreamer + mpph264enc）
- **优点：** 简单，GStreamer封装了复杂API
- **缺点：** 灵活性差，无法精细控制
- **延迟：** 约5-10ms（GStreamer开销）

### 6.2 新方案（直接调用MPP API）
- **优点：** 完全控制，可优化性能
- **缺点：** 代码复杂，需要处理细节
- **延迟：** 约2-5ms（减少GStreamer开销）

### 6.3 H265 vs H264
- **H265优势：** 同等画质下码率减少50%
- **H265劣势：** 编码复杂度略高，解码需要更多计算
- **推荐：** 带宽受限场景使用H265，延迟敏感场景使用H264

---

## 七、实现步骤

### 步骤1：创建MPP封装类
1. 创建 `mpp_encoder.h/cpp`
2. 创建 `mpp_decoder.h/cpp`
3. 测试基本编码解码功能

### 步骤2：集成到推流线程
1. 修改 `streamer_thread.cpp`
2. 替换GStreamer管线为MPP直接编码
3. 测试推流功能

### 步骤3：优化性能
1. 实现零拷贝（使用MPP缓冲区）
2. 实现多线程编码
3. 性能测试和调优

### 步骤4：添加解码支持
1. 实现RTSP拉流解码
2. 集成到推理流程
3. 端到端测试

---

## 八、注意事项

### 8.1 内存管理
- 使用 `mpp_buffer_get/put` 管理MPP缓冲区
- 避免频繁的内存分配和释放
- 使用内存池预分配缓冲区

### 8.2 错误处理
- 检查所有MPP API返回值
- 实现重试机制
- 记录详细的错误日志

### 8.3 线程安全
- MPP编码器不是线程安全的
- 每个线程需要独立的编码器实例
- 使用锁保护共享资源

### 8.4 性能优化
- 使用DMA缓冲区减少拷贝
- 实现异步编码
- 使用硬件加速的色彩空间转换

---

**文档生成工具：** Claude Code
**参考文档：** 瑞芯微MPP官方文档
**下次更新：** 待定
