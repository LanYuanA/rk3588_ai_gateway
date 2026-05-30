#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <opencv2/opencv.hpp>
#include "rknn_api.h"

// 定义人脸（目标）检测结果结构体
struct DetectResult {
    cv::Rect box;
    float confidence;
    int classId;
};

class RKNNDetector {
private:
    rknn_context ctx;
    unsigned char *model_data;
    bool is_init;

    rknn_input_output_num io_num;
    std::vector<rknn_tensor_attr> input_attrs;
    std::vector<rknn_tensor_attr> output_attrs;

    // RGA 预处理缓冲区（每线程独立，BGR→RGB + resize 一步完成）
    bool rga_preprocess_ok = false;
    bool rga_fused_off = false;
    void* rga_src_buf = nullptr;
    size_t rga_src_size = 0;
    void* rga_rgb_buf = nullptr;  // RGB 640×640 输出缓冲区
    size_t rga_rgb_size = 0;

    // 非极大值抑制 (NMS) 函数
    void nms(std::vector<DetectResult>& input_boxes, float nms_thresh);

    // RGA 预处理：BGR 640×480 → RGB 640×640
    bool rgaPreprocess(cv::Mat& bgr_frame, int target_w, int target_h);

public:
    RKNNDetector();
    ~RKNNDetector();

    // 加载 RKNN 模型并初始化 NPU 资源，npu_core_index 取值 0/1/2，其他值表示自动
    bool init(const std::string& model_path, int npu_core_index = -1);

    // 运行一次推理运算，并返回解析后的人脸框坐标集合
    std::vector<DetectResult> inference(cv::Mat& image);
};
