# RK3588 AI视频网关项目视频输出参数说明

根据源代码分析，本项目的视频输出参数如下：

## 视频输出参数

### 分辨率
- **输出分辨率**: 1280×960像素
- **拼接布局**: 4路视频流以2×2网格形式拼接
  - 每个子画面分辨率为 640×480 像素
  - 总体输出分辨率为 1280×960 像素

### 帧率
- **输出帧率**: 30帧/秒 (framerate=30/1)
- **实际处理帧率**: 约30帧/秒（每33毫秒处理一帧）

### 码率
- **目标码率**: 4,000,000 bps (4 Mbps)
- **码率控制模式**: CBR (恒定比特率, rc-mode=cbr)
- **GOP大小**: 60帧 (Group of Pictures)

## GStreamer管道配置

视频输出使用GStreamer MPP硬件编码器，具体配置如下：

```
appsrc is-live=true do-timestamp=true format=time block=false 
! queue leaky=downstream max-size-buffers=2 
! video/x-raw,format=BGR,width=1280,height=960,framerate=30/1 
! videoconvert 
! queue leaky=downstream max-size-buffers=2 
! mpph264enc bps=4000000 rc-mode=cbr gop=60 
! h264parse config-interval=1 
! rtspclientsink location=rtsp://127.0.0.1:8554/gateway_out protocols=tcp
```

## 编码器参数

- **编码器**: mpph264enc (Rockchip MPP H.264硬件编码器)
- **编码格式**: H.264/AVC
- **码率控制**: CBR (恒定比特率)
- **目标比特率**: 4,000,000 bps
- **GOP结构**: 关键帧间隔为60帧 (约2秒关键帧间隔)

## 备用输出配置

当硬件编码不可用时，系统会切换到备用输出：
- **备用格式**: OpenCV RAW MJPG
- **备用帧率**: 15帧/秒
- **备用分辨率**: 1280×960像素
- **文件名**: debug_4ch_output.avi

## 视频处理流程

1. 四路输入视频流分别处理并存储在画布(canvas)中
2. 每33毫秒将四路视频按2×2网格拼接成最终画面
3. 通过GStreamer管道将拼接后的画面送入MPP硬件编码器
4. 编码后的H.264流通过RTSP协议传输到指定地址

## RTSP输出地址

- **RTSP服务器地址**: rtsp://127.0.0.1:8554/gateway_out
- **传输协议**: TCP

这些参数确保了高质量的视频输出，同时利用RK3588的MPP硬件编码能力实现高效、低延迟的视频处理。