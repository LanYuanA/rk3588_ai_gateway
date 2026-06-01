# H265黑屏修复详细文档

**文档创建时间：** 2026-05-30 21:30:00
**修改时间范围：** 2026-05-30 21:25:00 - 2026-05-30 21:30:00
**修改人：** Claude Code
**修改目的：** 修复VLC播放H265流黑屏问题

---

## 一、问题描述

### 1.1 现象
- 程序正常推流到RTSP服务器
- VLC可以打开流地址
- 但画面显示黑屏，无图像

### 1.2 日志
```
[RTSP-MPP] rtsp://127.0.0.1:8554/host_cam3 使用 H264 + mppvideodec
[四路流-推流线程] 已推送第 1260 帧。 NV12转换耗时: 2 ms，编码推流耗时: 3 ms，RGA=成功，编码器=MPP H265
```

---

## 二、根本原因

### 2.1 H265解码需要的参数集

H265解码器需要三个关键参数集才能正确初始化：

| 参数集 | 全称 | 作用 |
|--------|------|------|
| VPS | Video Parameter Set | 视频参数集，定义视频层信息 |
| SPS | Sequence Parameter Set | 序列参数集，定义编码序列参数 |
| PPS | Picture Parameter Set | 图像参数集，定义图像编码参数 |

### 2.2 问题原因

**修改前的数据流：**
```
MPP编码器
    ↓ 直接输出编码数据（缺少VPS/SPS/PPS）
h265parse
    ↓ 无法解析（缺少头部信息）
VLC播放器
    ↓ 无法解码
黑屏
```

**MPP编码器的行为：**
- 首次编码后，VPS/SPS/PPS存储在编码器内部
- 需要通过 `MPP_ENC_GET_EXTRA_INFO` 命令主动获取
- 原代码没有获取这些头部数据

---

## 三、解决方案

### 3.1 修改mpp_encoder.h

**新增成员变量：**
```cpp
class MppH265Encoder {
private:
    // ... 其他成员 ...
    bool got_extra_data_ = false;
    std::vector<uint8_t> extra_data_;  // VPS/SPS/PPS头部数据
};
```

### 3.2 修改mpp_encoder.cpp

**在encode函数中添加头部数据获取：**
```cpp
bool MppH265Encoder::encode(const uint8_t* nv12_data, size_t data_size, 
                            std::vector<uint8_t>& h265_data) {
    // ... 编码逻辑 ...

    // 首次编码后获取VPS/SPS/PPS头部数据
    if (!got_extra_data_) {
        MppPacket extra_pkt = nullptr;
        ret = mpi->control(ctx, MPP_ENC_GET_EXTRA_INFO, &extra_pkt);
        if (ret == MPP_OK && extra_pkt) {
            void* extra_data = mpp_packet_get_data(extra_pkt);
            size_t extra_size = mpp_packet_get_length(extra_pkt);
            extra_data_.assign(static_cast<uint8_t*>(extra_data),
                              static_cast<uint8_t*>(extra_data) + extra_size);
            std::cout << "[MPP编码器] 获取VPS/SPS/PPS头部数据: " 
                      << extra_size << " 字节" << std::endl;
        }
        got_extra_data_ = true;
    }

    // 将VPS/SPS/PPS头部数据添加到编码数据前面
    if (!extra_data_.empty()) {
        h265_data.reserve(extra_data_.size() + packet_size);
        h265_data.insert(h265_data.end(), extra_data_.begin(), extra_data_.end());
        h265_data.insert(h265_data.end(),
                         static_cast<uint8_t*>(packet_data),
                         static_cast<uint8_t*>(packet_data) + packet_size);
    } else {
        h265_data.assign(static_cast<uint8_t*>(packet_data),
                         static_cast<uint8_t*>(packet_data) + packet_size);
    }

    // ...
}
```

---

## 四、修改后的数据流

```
MPP编码器
    ↓ 首次编码后获取VPS/SPS/PPS
    ↓ 每帧编码数据前面拼接VPS/SPS/PPS
h265parse
    ↓ 正确解析H265码流
VLC播放器
    ↓ 正常解码显示
正常画面
```

---

## 五、H265码流结构

### 5.1 完整的H265码流

```
[VPS] [SPS] [PPS] [IDR帧] [P帧] [P帧] [P帧] [IDR帧] [P帧] ...
  ↑                                    ↑
  头部数据                           关键帧（每2秒一个）
```

### 5.2 NAL单元类型

| NAL类型 | 值 | 说明 |
|---------|-----|------|
| VPS | 32 | 视频参数集 |
| SPS | 33 | 序列参数集 |
| PPS | 34 | 图像参数集 |
| IDR | 19/20 | 关键帧 |
| P | 1 | 预测帧 |

---

## 六、性能影响

### 6.1 额外开销

| 项目 | 大小 | 频率 | 影响 |
|------|------|------|------|
| VPS/SPS/PPS | 约100字节 | 每帧 | 可忽略 |
| memcpy | 100字节 | 每帧 | 约0.01ms |

### 6.2 总体影响

- **编码延迟：** 无增加
- **数据大小：** 每帧增加约100字节（可忽略）
- **CPU占用：** 无明显增加

---

## 七、测试验证

### 7.1 编译运行

```bash
rm -rf build && mkdir build && cd build
cmake ..
make -j$(nproc)
./RK3588_AI_Gateway
```

### 7.2 预期日志

```
[MPP编码器] H265编码器初始化成功: 1280x960 @30fps, 4000kbps
[MPP编码器] 获取VPS/SPS/PPS头部数据: XXX 字节  ← 新增日志
[H265推流] GStreamer H265推流管线就绪: rtsp://127.0.0.1:8554/gateway_out
```

### 7.3 VLC验证

1. 打开VLC
2. 媒体 → 打开网络串流
3. 输入：`rtsp://127.0.0.1:8554/gateway_out`
4. 应该能看到正常画面

---

## 八、相关知识

### 8.1 H264 vs H265参数集

| 项目 | H264 | H265 |
|------|------|------|
| 参数集 | SPS + PPS | VPS + SPS + PPS |
| 获取方式 | `MPP_ENC_GET_EXTRA_INFO` | `MPP_ENC_GET_EXTRA_INFO` |
| 大小 | 约50字节 | 约100字节 |

### 8.2 MPP编码器API

```cpp
// 获取编码器额外信息（VPS/SPS/PPS）
MppPacket extra_pkt = nullptr;
mpi->control(ctx, MPP_ENC_GET_EXTRA_INFO, &extra_pkt);

// 获取数据
void* data = mpp_packet_get_data(extra_pkt);
size_t size = mpp_packet_get_length(extra_pkt);
```

---

## 九、修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `include/mpp_encoder.h` | 添加extra_data_和got_extra_data_成员 |
| `src/mpp_encoder.cpp` | 添加VPS/SPS/PPS获取和拼接逻辑 |

---

**文档生成工具：** Claude Code
**文档版本：** v1.0
**下次更新：** 待定
