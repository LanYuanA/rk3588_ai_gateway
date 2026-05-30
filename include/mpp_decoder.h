#pragma once

#include <cstdint>
#include <vector>
#include <opencv2/opencv.hpp>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

/**
 * @brief MPP H265解码器封装类
 * 直接调用瑞芯微MPP库进行H265硬件解码
 */
class MppH265Decoder {
public:
    MppH265Decoder();
    ~MppH265Decoder();

    bool init();
    bool decode(const std::vector<uint8_t>& h265_data, cv::Mat& nv12_frame);
    bool decode(const uint8_t* h265_data, size_t data_size, cv::Mat& nv12_frame);
    void release();
    bool isInitialized() const { return initialized_; }
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }

private:
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;

    int width_ = 0;
    int height_ = 0;
    bool initialized_ = false;
    bool info_changed_ = false;

    MppH265Decoder(const MppH265Decoder&) = delete;
    MppH265Decoder& operator=(const MppH265Decoder&) = delete;
};
