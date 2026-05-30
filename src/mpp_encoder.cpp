#include "mpp_encoder.h"

#include <iostream>
#include <cstring>

MppH265Encoder::MppH265Encoder() = default;

MppH265Encoder::~MppH265Encoder() {
    release();
}

bool MppH265Encoder::init(int width, int height, int fps, int bitrate) {
    if (initialized_) {
        std::cerr << "[MPP编码器] 已经初始化，先释放再重新初始化" << std::endl;
        release();
    }

    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_ = bitrate;

    // 1. 创建编码器上下文
    MPP_RET ret = mpp_create(&ctx, &mpi);
    if (ret != MPP_OK) {
        std::cerr << "[MPP编码器] 创建编码器失败: " << ret << std::endl;
        return false;
    }

    // 2. 初始化编码器（H265类型）
    ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC);
    if (ret != MPP_OK) {
        std::cerr << "[MPP编码器] 初始化编码器失败: " << ret << std::endl;
        release();
        return false;
    }

    // 3. 获取默认配置
    ret = mpp_enc_cfg_init(&cfg);
    if (ret != MPP_OK) {
        std::cerr << "[MPP编码器] 初始化配置失败: " << ret << std::endl;
        release();
        return false;
    }

    // 4. 配置编码参数
    mpp_enc_cfg_set_s32(cfg, "prep:width", width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", width);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", height);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_VBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bitrate);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bitrate * 3 / 2);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bitrate / 2);

    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);

    mpp_enc_cfg_set_s32(cfg, "rc:gop", fps * 2);

    // 5. 应用配置
    ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    if (ret != MPP_OK) {
        std::cerr << "[MPP编码器] 应用配置失败: " << ret << std::endl;
        release();
        return false;
    }

    // 6. 分配缓冲区
    size_t buf_size = width * height * 3 / 2;
    ret = mpp_buffer_get(nullptr, &buffer, buf_size);
    if (ret != MPP_OK) {
        std::cerr << "[MPP编码器] 分配缓冲区失败: " << ret << std::endl;
        release();
        return false;
    }

    initialized_ = true;
    std::cout << "[MPP编码器] H265编码器初始化成功: " << width << "x" << height
              << " @" << fps << "fps, " << bitrate/1000 << "kbps" << std::endl;
    return true;
}

bool MppH265Encoder::encode(const cv::Mat& nv12_frame, std::vector<uint8_t>& h265_data) {
    if (!initialized_) return false;
    if (nv12_frame.empty()) return false;

    size_t expected_size = width_ * height_ * 3 / 2;
    if (nv12_frame.total() * nv12_frame.elemSize() < expected_size) {
        std::cerr << "[MPP编码器] 输入帧尺寸不足" << std::endl;
        return false;
    }

    return encode(nv12_frame.data, expected_size, h265_data);
}

bool MppH265Encoder::encode(const uint8_t* nv12_data, size_t data_size, std::vector<uint8_t>& h265_data) {
    if (!initialized_ || !ctx || !mpi || !buffer) return false;

    // 1. 创建MppFrame
    MppFrame frame = nullptr;
    MPP_RET ret = mpp_frame_init(&frame);
    if (ret != MPP_OK) {
        std::cerr << "[MPP编码器] 创建帧失败: " << ret << std::endl;
        return false;
    }

    mpp_frame_set_width(frame, width_);
    mpp_frame_set_height(frame, height_);
    mpp_frame_set_hor_stride(frame, width_);
    mpp_frame_set_ver_stride(frame, height_);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, buffer);

    // 2. 复制数据到缓冲区
    void* buf_ptr = mpp_buffer_get_ptr(buffer);
    memcpy(buf_ptr, nv12_data, data_size);

    // 3. 送入编码器
    ret = mpi->encode_put_frame(ctx, frame);
    if (ret != MPP_OK) {
        std::cerr << "[MPP编码器] 送入编码器失败: " << ret << std::endl;
        mpp_frame_deinit(&frame);
        return false;
    }

    // 4. 获取编码后的数据
    MppPacket packet = nullptr;
    ret = mpi->encode_get_packet(ctx, &packet);
    if (ret != MPP_OK || !packet) {
        std::cerr << "[MPP编码器] 获取编码数据失败: " << ret << std::endl;
        mpp_frame_deinit(&frame);
        return false;
    }

    // 5. 复制编码数据
    void* packet_data = mpp_packet_get_data(packet);
    size_t packet_size = mpp_packet_get_length(packet);
    h265_data.assign(static_cast<uint8_t*>(packet_data),
                     static_cast<uint8_t*>(packet_data) + packet_size);

    // 6. 释放资源
    mpp_packet_deinit(&packet);
    mpp_frame_deinit(&frame);

    return true;
}

void MppH265Encoder::release() {
    if (ctx && mpi) {
        mpi->reset(ctx);
        mpp_destroy(ctx);
        ctx = nullptr;
        mpi = nullptr;
    }
    if (cfg) {
        mpp_enc_cfg_deinit(cfg);
        cfg = nullptr;
    }
    if (buffer) {
        mpp_buffer_put(buffer);
        buffer = nullptr;
    }
    initialized_ = false;
}
