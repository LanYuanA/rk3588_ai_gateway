#include "mpp_decoder.h"

#include <iostream>
#include <cstring>

MppH265Decoder::MppH265Decoder() = default;

MppH265Decoder::~MppH265Decoder() {
    release();
}

bool MppH265Decoder::init() {
    if (initialized_) {
        std::cerr << "[MPP解码器] 已经初始化，先释放再重新初始化" << std::endl;
        release();
    }

    // 1. 创建解码器上下文
    MPP_RET ret = mpp_create(&ctx, &mpi);
    if (ret != MPP_OK) {
        std::cerr << "[MPP解码器] 创建解码器失败: " << ret << std::endl;
        return false;
    }

    // 2. 初始化解码器（H265类型）
    ret = mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingHEVC);
    if (ret != MPP_OK) {
        std::cerr << "[MPP解码器] 初始化解码器失败: " << ret << std::endl;
        release();
        return false;
    }

    initialized_ = true;
    std::cout << "[MPP解码器] H265解码器初始化成功" << std::endl;
    return true;
}

bool MppH265Decoder::decode(const std::vector<uint8_t>& h265_data, cv::Mat& nv12_frame) {
    return decode(h265_data.data(), h265_data.size(), nv12_frame);
}

bool MppH265Decoder::decode(const uint8_t* h265_data, size_t data_size, cv::Mat& nv12_frame) {
    if (!initialized_ || !ctx || !mpi) return false;

    // 1. 创建MppPacket
    MppPacket packet = nullptr;
    // mpp_packet_init 需要 void*，需要 const_cast
    MPP_RET ret = mpp_packet_init(&packet, const_cast<uint8_t*>(h265_data), data_size);
    if (ret != MPP_OK) {
        std::cerr << "[MPP解码器] 创建包失败: " << ret << std::endl;
        return false;
    }
    mpp_packet_set_pts(packet, 0);

    // 2. 送入解码器
    ret = mpi->decode_put_packet(ctx, packet);
    if (ret != MPP_OK) {
        std::cerr << "[MPP解码器] 送入解码器失败: " << ret << std::endl;
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
    int info_change = mpp_frame_get_info_change(frame);
    if (info_change) {
        width_ = mpp_frame_get_width(frame);
        height_ = mpp_frame_get_height(frame);
        std::cout << "[MPP解码器] 解码信息变化: " << width_ << "x" << height_ << std::endl;
        info_changed_ = true;

        mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        mpp_frame_deinit(&frame);
        mpp_packet_deinit(&packet);
        return false;
    }

    // 5. 获取解码数据
    int width = mpp_frame_get_width(frame);
    int height = mpp_frame_get_height(frame);
    int hor_stride = mpp_frame_get_hor_stride(frame);
    int ver_stride = mpp_frame_get_ver_stride(frame);

    MppBuffer buf = mpp_frame_get_buffer(frame);
    void* buf_ptr = mpp_buffer_get_ptr(buf);

    // 6. 复制到cv::Mat
    if (hor_stride == width && ver_stride == height) {
        size_t frame_size = width * height * 3 / 2;
        nv12_frame.create(height * 3 / 2, width, CV_8UC1);
        memcpy(nv12_frame.data, buf_ptr, frame_size);
    } else {
        // 有stride，需要逐行复制
        nv12_frame.create(height * 3 / 2, width, CV_8UC1);

        const uint8_t* src_y = static_cast<const uint8_t*>(buf_ptr);
        uint8_t* dst_y = nv12_frame.data;
        for (int i = 0; i < height; i++) {
            memcpy(dst_y + i * width, src_y + i * hor_stride, width);
        }

        const uint8_t* src_uv = src_y + hor_stride * ver_stride;
        uint8_t* dst_uv = dst_y + width * height;
        for (int i = 0; i < height / 2; i++) {
            memcpy(dst_uv + i * width, src_uv + i * hor_stride, width);
        }
    }

    width_ = width;
    height_ = height;

    // 7. 释放资源
    mpp_frame_deinit(&frame);
    mpp_packet_deinit(&packet);

    return true;
}

void MppH265Decoder::release() {
    if (ctx && mpi) {
        mpi->reset(ctx);
        mpp_destroy(ctx);
        ctx = nullptr;
        mpi = nullptr;
    }
    initialized_ = false;
    width_ = 0;
    height_ = 0;
}
