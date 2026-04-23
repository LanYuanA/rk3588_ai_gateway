#include "puller_thread.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits.h>
#include <random>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <linux/videodev2.h>

#include "app_context.h"
#include "rga_resize.h"
#include "rga_yuv_converter.h"
#include "rtsp_mpp_decoder.h"
#include "time_manager.h"

namespace {

bool envFlagEnabled(const char* key, bool default_value = false) {
    const char* value = std::getenv(key);
    if (!value || value[0] == '\0') {
        return default_value;
    }
    return std::strcmp(value, "1") == 0 ||
           std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "TRUE") == 0 ||
           std::strcmp(value, "yes") == 0 ||
           std::strcmp(value, "YES") == 0;
}

int envIntValue(const char* key, int default_value) {
    const char* value = std::getenv(key);
    if (!value || value[0] == '\0') {
        return default_value;
    }
    char* endptr = nullptr;
    long parsed = std::strtol(value, &endptr, 10);
    if (endptr == value) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

std::string envStringValue(const char* key, const std::string& default_value) {
    const char* value = std::getenv(key);
    if (!value || value[0] == '\0') {
        return default_value;
    }
    return std::string(value);
}

std::string getExecutableDir() {
    char exe_path[PATH_MAX] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) {
        return ".";
    }
    exe_path[len] = '\0';
    std::string full_path(exe_path);
    size_t last_slash = full_path.find_last_of('/');
    if (last_slash == std::string::npos) {
        return ".";
    }
    return full_path.substr(0, last_slash);
}

std::string parentDir(const std::string& path) {
    size_t last_slash = path.find_last_of('/');
    if (last_slash == std::string::npos || last_slash == 0) {
        return ".";
    }
    return path.substr(0, last_slash);
}

std::string getProjectRootDir() {
    const std::string exe_dir = getExecutableDir();
    std::string root = parentDir(exe_dir);
    if (root.empty() || root == ".") {
        char cwd[PATH_MAX] = {0};
        if (getcwd(cwd, sizeof(cwd)) != nullptr) {
            return std::string(cwd);
        }
        return ".";
    }
    return root;
}

struct V4L2MappedBuffer {
    void* start = nullptr;
    size_t length = 0;
};

class V4L2Capture {
public:
    V4L2Capture() = default;
    ~V4L2Capture() {
        close();
    }

    bool open(const std::string& device, int req_width, int req_height) {
        close();

        fd_ = ::open(device.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd_ < 0) {
            std::perror("[V4L2] open");
            return false;
        }

        v4l2_capability cap_info{};
        if (ioctl(fd_, VIDIOC_QUERYCAP, &cap_info) < 0) {
            std::perror("[V4L2] VIDIOC_QUERYCAP");
            close();
            return false;
        }

        if (!(cap_info.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
            !(cap_info.capabilities & V4L2_CAP_STREAMING)) {
            std::cerr << "[V4L2] 设备不支持视频采集或流式传输: " << device << std::endl;
            close();
            return false;
        }

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = req_width;
        fmt.fmt.pix.height = req_height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            std::perror("[V4L2] VIDIOC_S_FMT");
            close();
            return false;
        }

        width_ = fmt.fmt.pix.width;
        height_ = fmt.fmt.pix.height;
        bytes_per_line_ = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : width_ * 2;

        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
            std::perror("[V4L2] VIDIOC_REQBUFS");
            close();
            return false;
        }

        if (req.count < 2) {
            std::cerr << "[V4L2] 缓冲区数量不足" << std::endl;
            close();
            return false;
        }

        buffers_.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                std::perror("[V4L2] VIDIOC_QUERYBUF");
                close();
                return false;
            }

            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED) {
                std::perror("[V4L2] mmap");
                close();
                return false;
            }

            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
                std::perror("[V4L2] VIDIOC_QBUF");
                close();
                return false;
            }
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
            std::perror("[V4L2] VIDIOC_STREAMON");
            close();
            return false;
        }

        streaming_ = true;
        
        return true;
    }

    bool read(cv::Mat& bgr_frame) {
        if (fd_ < 0) {
            return false;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);

        timeval tv{};
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        int ret = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                return false;
            }
            std::perror("[V4L2] select");
            return false;
        }
        if (ret == 0) {
            std::cerr << "[V4L2] 读取超时" << std::endl;
            return false;
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno != EAGAIN) {
                std::perror("[V4L2] VIDIOC_DQBUF");
            }
            return false;
        }

        // 尝试使用RGA进行YUV到BGR的转换
        bool converted_by_rga = tryConvertYuvToBgrWithRga(
            buffers_[buf.index].start,
            width_,
            height_,
            bytes_per_line_,
            bgr_frame,
            "/dev/dma_heap/system-dma32",  // 默认DMA堆路径
            yuv_converter_context_
        );

        if (!converted_by_rga) {
            // 如果RGA转换失败，回退到OpenCV转换
            cv::Mat yuyv_frame(height_, width_, CV_8UC2, buffers_[buf.index].start, bytes_per_line_);
            cv::cvtColor(yuyv_frame, bgr_frame, cv::COLOR_YUV2BGR_YUYV);
        }

        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            std::perror("[V4L2] VIDIOC_QBUF(requeue)");
            return false;
        }

        return true;
    }

    void close() {
        if (fd_ >= 0) {
            if (streaming_) {
                v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                ioctl(fd_, VIDIOC_STREAMOFF, &type);
            }

            for (auto& buffer : buffers_) {
                if (buffer.start && buffer.start != MAP_FAILED) {
                    munmap(buffer.start, buffer.length);
                }
            }
            buffers_.clear();

            ::close(fd_);
            fd_ = -1;
        }

        streaming_ = false;
        width_ = 0;
        height_ = 0;
        bytes_per_line_ = 0;
    }

    // 公共访问器，用于初始化RGA YUV转换器上下文
    RgaYuvConverterContext& getYuvConverterContext() {
        return yuv_converter_context_;
    }

private:
    int fd_ = -1;
    bool streaming_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t bytes_per_line_ = 0;
    std::vector<V4L2MappedBuffer> buffers_;
    RgaYuvConverterContext yuv_converter_context_;
};

} // namespace

void streamPullerAndDecoderThread(int streamId, const std::string& stream_url) {
    std::cout << "[拉流/解码线程 " << streamId << "] 准备拉取媒体源: " << stream_url << std::endl;

    const bool is_v4l2_stream = (stream_url.find("/dev/video") != std::string::npos);
    const bool is_rtsp_stream = (stream_url.find("rtsp://") != std::string::npos);
    const bool use_rga_resize = envFlagEnabled("RK_USE_RGA_RESIZE", true);
    const bool use_dma32_for_rga = envFlagEnabled("RK_RGA_USE_DMA32", true);
    const std::string dma_heap_path = envStringValue("RK_DMA_HEAP_PATH", "/dev/dma_heap/system-dma32");
    const int rga_scheduler_mode = envIntValue("RK_RGA_SCHEDULER", -1);
    int reconnect_backoff_ms = 500;
    const int reconnect_backoff_max_ms = 30000;
    std::mt19937 rng(static_cast<unsigned int>(
        std::chrono::steady_clock::now().time_since_epoch().count() + streamId * 131));
    std::uniform_int_distribution<int> jitter_dist(0, 200);

    V4L2Capture v4l2_cap;
    RtspMppDecoder rtsp_decoder;
    cv::VideoCapture cap;
    bool use_rtsp_mpp = false;
    if (is_v4l2_stream) {
        if (!v4l2_cap.open(stream_url, 640, 480)) {
            std::cerr << "[错误] V4L2 打开摄像头失败: " << stream_url << std::endl;
            return;
        }
         // 初始化V4L2捕获器的RGA YUV转换器上下文，只使用RGA3核心
         int rga3_only_mode = 0; // 强制使用RGA3_CORE0
         initializeRgaYuvConverterContext(streamId,
                                        true,  // use_rga_conversion
                                        use_dma32_for_rga,
                                        rga3_only_mode,
                                        v4l2_cap.getYuvConverterContext());
    } else if (stream_url.find("rtsp://") != std::string::npos) {
        if (rtsp_decoder.open(stream_url)) {
            use_rtsp_mpp = true;
        } else {
            std::cerr << "[警告] RTSP MPP 打开失败，回退 FFmpeg 软解: " << stream_url << std::endl;
            setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp|stimeout;2000000|flags;low_delay|video_codec;h264", 1);
            cap.open(stream_url, cv::CAP_FFMPEG);
            if (!cap.isOpened()) {
                std::cerr << "[错误] RTSP 软解也打开失败: " << stream_url << std::endl;
                return;
            }
        }
    } else {
        std::string local_file_uri = "file://" + getProjectRootDir() + "/" + stream_url;
        std::string gst_pipeline = "uridecodebin uri=" + local_file_uri + " ! videoconvert ! appsink sync=false max-buffers=5";
        cap.open(gst_pipeline, cv::CAP_GSTREAMER);
        if (!cap.isOpened()) {
            std::cout << "[警告] 启用 GStreamer 加速播放文件失败，回退到普通读取..." << std::endl;
            cap.open(stream_url);
        }
    }

    if (!is_v4l2_stream) {
        if (!is_rtsp_stream && !cap.isOpened()) {
            std::cerr << "[错误] 无法打开媒体源 " << streamId << ": " << stream_url << std::endl;
            return;
        }
    }

    uint64_t frame_seq = 0;
    cv::Mat bgr_frame;
    cv::Mat nv12_frame;
    auto last_read_time = std::chrono::steady_clock::now();
    RgaResizeContext rga_context;
    int rga3_only_mode = 0; // 强制使用RGA3_CORE0
    initializeRgaResizeContext(streamId,
                               use_rga_resize,
                               use_dma32_for_rga,
                               rga3_only_mode,
                               rga_context);
    RgaYuvConverterContext rtsp_rga_yuv_context;
    if (is_rtsp_stream) {
        int rga3_only_mode = 0; // 强制使用RGA3_CORE0
        initializeRgaYuvConverterContext(streamId,
                                       true,
                                       use_dma32_for_rga,
                                       rga3_only_mode,
                                       rtsp_rga_yuv_context);
    }
     while (g_system_running) {
         bool ret = false;
         if (is_v4l2_stream) {
             ret = v4l2_cap.read(bgr_frame);
         } else if (is_rtsp_stream) {
             cv::Mat nv12_frame;
             if (use_rtsp_mpp) {
                 int64_t dummy_timestamp;
                 ret = rtsp_decoder.read(nv12_frame, dummy_timestamp);
                 if (ret && !nv12_frame.empty()) {
                      // NV12格式：Y分量占前H行，UV分量交错排列占后H/2行
                      // 因此总行数是H + H/2 = 3H/2，所以原始高度是rows * 2 / 3
                      int raw_height = nv12_frame.rows * 2 / 3;  // 实际Y分量的高度
                      bool converted_by_rga = convertYuv420ToBgrWithRga(
                          nv12_frame.data,
                          nv12_frame.cols,
                          raw_height,
                          bgr_frame,
                          dma_heap_path,
                          rtsp_rga_yuv_context);

                     if (!converted_by_rga) {
                         cv::Mat nv12_view(raw_height * 3 / 2, nv12_frame.cols, CV_8UC1, nv12_frame.data);
                         cv::cvtColor(nv12_view, bgr_frame, cv::COLOR_YUV2BGR_NV12);
                     }
                 }
             } else {
                 ret = cap.read(bgr_frame);
             }
         } else {
             ret = cap.read(bgr_frame);
         }
        if (!ret || bgr_frame.empty()) {
            int wait_ms = reconnect_backoff_ms + jitter_dist(rng);
            std::cerr << "[警告] 流 " << streamId << " 读取失败，" << wait_ms
                      << "ms 后重连..." << std::endl;
            if (is_v4l2_stream) {
                v4l2_cap.close();
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                if (v4l2_cap.open(stream_url, 640, 480)) {
                    reconnect_backoff_ms = 500;
                } else {
                    std::cerr << "[错误] V4L2 重连失败: " << stream_url << std::endl;
                    reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, reconnect_backoff_max_ms);
                }
                continue;
            }
            if (is_rtsp_stream) {
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));

                if (use_rtsp_mpp) {
                    rtsp_decoder.close();
                    if (rtsp_decoder.open(stream_url)) {
                        reconnect_backoff_ms = 500;
                    } else {
                        // MPP 失效后自动降级到软解，避免整路中断。
                        use_rtsp_mpp = false;
                        setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp|stimeout;2000000|flags;low_delay|video_codec;h264", 1);
                        cap.open(stream_url, cv::CAP_FFMPEG);
                        if (cap.isOpened()) {
                            std::cerr << "[警告] RTSP MPP 重连失败，已切换 FFmpeg 软解: " << stream_url << std::endl;
                            reconnect_backoff_ms = 500;
                        } else {
                            reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, reconnect_backoff_max_ms);
                        }
                    }
                } else {
                    cap.release();
                    cap.open(stream_url, cv::CAP_FFMPEG);
                    if (cap.isOpened()) {
                        reconnect_backoff_ms = 500;
                    } else {
                        reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, reconnect_backoff_max_ms);
                    }
                }
            } else {
                cap.set(cv::CAP_PROP_POS_FRAMES, 0);
                reconnect_backoff_ms = 500;
            }
            continue;
        }

        reconnect_backoff_ms = 500;

        auto steady_now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(steady_now - last_read_time).count();

        bool is_live_stream = (is_rtsp_stream || is_v4l2_stream);
        if (!is_live_stream && elapsed < 32) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33 - elapsed));
        }
        last_read_time = std::chrono::steady_clock::now();

        if (bgr_frame.cols != 640 || bgr_frame.rows != 480) {
            bool resized_by_rga = tryResizeTo640x480WithRga(bgr_frame, dma_heap_path, rga_context);
            if (!resized_by_rga) {
                cv::resize(bgr_frame, bgr_frame, cv::Size(640, 480));
            }
        }

         VideoFrame frame;
         frame.stream_id = streamId;
         frame.frame_id = frame_seq++;
         // 移除时间戳获取以提高性能，只在拼接线程中添加时间显示
         frame.timestamp_ms = 0;  // 设置为0表示不使用时间戳同步
         frame.image = bgr_frame.clone();

        g_inference_queues[streamId].push(frame);
        g_push_queues[streamId].push(frame);
    }

    if (is_v4l2_stream) {
        v4l2_cap.close();
    } else if (is_rtsp_stream) {
        if (use_rtsp_mpp) {
            rtsp_decoder.close();
        } else {
            cap.release();
        }
    } else {
        cap.release();
    }
    std::cout << "[拉流/解码线程 " << streamId << "] 退出." << std::endl;
}
