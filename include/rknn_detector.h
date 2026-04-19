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

    // 非极大值抑制 (NMS) 函数
    void nms(std::vector<DetectResult>& input_boxes, float nms_thresh);

public:
    RKNNDetector();
    ~RKNNDetector();

    // 加载 RKNN 模型并初始化 NPU 资源
    bool init(const std::string& model_path);

    // 运行一次推理运算，并返回解析后的人脸框坐标集合
    std::vector<DetectResult> inference(cv::Mat& image);
};
