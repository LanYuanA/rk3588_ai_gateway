#pragma once

#include <cstdint>
#include <vector>
#include <opencv2/opencv.hpp>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_venc_cmd.h>

/**
 * @brief MPP H265编码器封装类
 * 直接调用瑞芯微MPP库进行H265硬件编码
 */
class MppH265Encoder {
public:
    MppH265Encoder();
    ~MppH265Encoder();

    bool init(int width, int height, int fps, int bitrate);
    bool encode(const cv::Mat& nv12_frame, std::vector<uint8_t>& h265_data);
    bool encode(const uint8_t* nv12_data, size_t data_size, std::vector<uint8_t>& h265_data);
    void release();
    bool isInitialized() const { return initialized_; }

private:
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppEncCfg cfg = nullptr;
    MppBuffer buffer = nullptr;

    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    int bitrate_ = 4000000;
    bool initialized_ = false;
    bool got_extra_data_ = false;
    std::vector<uint8_t> extra_data_;  // VPS/SPS/PPS头部数据

    MppH265Encoder(const MppH265Encoder&) = delete;
    MppH265Encoder& operator=(const MppH265Encoder&) = delete;
};
