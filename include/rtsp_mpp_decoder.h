#pragma once

#include <cstdint>
#include <opencv2/opencv.hpp>
#include <string>

class RtspMppDecoder {
public:
    RtspMppDecoder() = default;
    ~RtspMppDecoder();

    bool open(const std::string& rtsp_url);
    bool read(cv::Mat& nv12_frame, int64_t& timestamp_ms);
    void close();

private:
    std::string rtsp_url_;
    void* pipeline_ = nullptr;
    void* appsink_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};
