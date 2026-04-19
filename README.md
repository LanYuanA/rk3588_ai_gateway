# RK3588 AI Video Gateway (多路 AI 视频融合网关)

🚀 **一个基于 RK3588 芯片的纯硬件加速多路视频 AI 处理与 RTSP 推流网关项目。**

本项目旨在充分榨干 Rockchip RK3588 的硬件性能（NPU、RGA、MPP），实现对多路视频流（USB摄像头、RTSP网络流、本地MP4等）的同时拉取、AI 目标检测（以 YOLOv8 Face 为例）、四宫格画面拼接，并最终通过硬件编码推送到 MediaMTX 流媒体服务器，全程维持极低的 CPU 占用率和极高的实时性。

---

## ✨ 核心特性 / Features

* **🧠 极致 NPU 推理加速 (RKNN)**
  采用瑞芯微 RKNN-Toolkit2，将 YOLOv8 等深度学习模型真正下发到板载 6TOPS NPU 上运行。项目内实现了多线程绑定实体 NPU Core（RK3588 拥有 3 个 NPU 核心）做并发推理，避免锁竞争。
* **⚡ RGA 硬件级缩放与拼接**
  弃用耗费 CPU 的 `cv::resize`，使用 RK3588 专属的 2D 图形加速引擎 (librga)，实现超低延迟的视频帧缩放、色彩空间转换与 2x2 四宫格画中画拼接。
* **🎬 MPP 硬件 H.264 编码 (mpph264enc)**
  利用 RK3588 的 VPU (Video Processing Unit)，摒弃软解软编。借助 GStreamer 的 `mpph264enc` 插件进行全硬件视频流压缩，不再让 CPU 温度爆表。
* **🌐 RTSP / RTMP 低延迟推流**
  无缝对接开源流媒体引擎 [MediaMTX](https://github.com/bluenviron/mediamtx)。通过精简组合 `appsrc ! videoconvert ! mpph264enc ! rtspclientsink` 管道，将处理完毕的安防/直播网格流反推给服务端，适用于大屏监控或云端分发。
* **🧵 高效多线程流水线 (Pipeline)**
  基于无锁或带锁的线程安全队列 (`ThreadSafeQueue`) 架构。将“视频拉取”、“深度学习推理”、“RGA 拼接”与“GStreamer 编码分发”完全解耦在不同线程中，帧缓冲平滑过渡。

---

## 🛠️ 系统系统要求与依赖 / Prerequisites

* **硬件**: 瑞芯微 RK3588 系列开发板 (如 Firefly, 边缘计算盒子等)
* **系统**: Ubuntu 20.04/22.04 LTS (ARM64)
* **核心依赖包**:
  * [OpenCV](https://opencv.org/) 4.x (需开启 GStreamer 编译支持)
  * GStreamer 1.16.3+ (需安装完整的基础库，并确保包含 `gstreamer1.0-rockchip` / MPP 插件)
  * **gst-rtsp-server** (如果系统缺失 `rtspclientsink`，需从源码编译对应的 tag 版本)
  * [librga](https://github.com/airockchip/librga) (瑞芯微 RGA 库)
  * [rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2) (C++ API: `librknnrt.so`)
* **流媒体服务器**: [MediaMTX](https://github.com/bluenviron/mediamtx) (一键式二进制服务)

---

## 🚀 快速开始 / Get Started

### 1. 编译项目
克隆本项目到 RK3588 开发板：

```bash
git clone https://github.com/YourName/RK3588_AI_Gateway.git
cd RK3588_AI_Gateway
mkdir build && cd build

# 生成 Makefile
cmake ..

# 开始编译 (RK3588 可支持 -j4 或 -j8)
make -j4
```

### 2. 启动流媒体服务器 (MediaMTX)
在运行网关之前，必须先将接收 RTSP 推流的本地或远程服务器运行起来：
```bash
# 下载并进入 MediaMTX 目录
cd mediamtx_server/
./mediamtx
```

### 3. 运行 AI 网关程序
打开**另一个终端（Terminal）**，执行编译好的网关网关文件。
它会自动分配线程连接视频源、进行 YOLOv8 人脸检测、RGA 合成、以及 GStreamer MPP 编码：

```bash
cd build
./RK3588_AI_Gateway
```
*看见 `[四路流-推流线程] 已同步推送第 x 帧` 代表运行成功！*

### 4. 客户端拉流验证
回到您的 PC 或任何有局域网连接的机器，打开 **VLC 播放器** 或 **OBS**：
> 文件 -> 打开网络串流 (Open Network Stream)...

输入以下地址（请将 IP 替换为您的 RK3588 开发板局域网 IP）：
```text
rtsp://<RK3588_IP>:8554/gateway_out
```

*(注意：若 VLC 播放出现丢包卡顿，请在 VLC 设置 `工具`->`首选项`->`全部`->`输入/编解码器`->`分离器`->`RTP/RTSP` 中勾选 **使用 RTP over RTSP (TCP)** 选项。)*

---

## 🏗️ 架构图 / Architecture Flow

1. **Pull Threads (输入层)**: FFmpeg / V4L2 多路解流 -> 转入帧队列。
2. **NPU Threads (处理层)**: 多核心轮询取帧 -> RKNN_Inference -> 返回含 Bounding Box 坐标的结果。
3. **Streamer Thread (合并发流层)**:
   * **画图**: 在原帧上使用 OpenCV `cv::rectangle` 标记检测框。
   * **缩放**: 调起 **librga.so** 将画面缩小。
   * **拼接**: 调起 **librga.so** 进行 2x2 `imcopy` 至全局 1280x960 画布。
   * **编码与推流**: 原生 BGR 画布交由 GStreamer -> `videoconvert` (转换为YUV格式) -> `mpph264enc` (VPU极限H.264压缩) -> `rtspclientsink` -> TCP网络 -> MediaMTX。

---

## 📝 待办事项 / TODO
- [ ] 支持动态增删视频源 (Dynamic stream add/remove)。
- [ ] 接入 SIP 协议，支持向 GB28181 平台（公安/国标平台）注册和级联。
- [ ] 支持在配置文件 (JSON/YAML) 中读取模型路径和视频源流地址。

---

## 📄 许可证 / License
[MIT License](LICENSE)

*如果本项目对你的 RK3588 边缘计算产品落地有帮助，欢迎点亮 ⭐️ Star!*