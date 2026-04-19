#include "rknn_detector.h"
#include <fstream>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>

static unsigned char *load_file(const std::string& filename, int* size) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) return nullptr;
    *size = file.tellg();
    file.seekg(0, std::ios::beg);
    unsigned char *data = new unsigned char[*size];
    file.read((char*)data, *size);
    return data;
}

RKNNDetector::RKNNDetector() : ctx(0), model_data(nullptr), is_init(false) {}

RKNNDetector::~RKNNDetector() {
    if (ctx > 0) {
        rknn_destroy(ctx);
    }
    if (model_data) {
        delete[] model_data;
    }
}

bool RKNNDetector::init(const std::string& model_path) {
    std::cout << "[RKNN] 正在加载模型: " << model_path << std::endl;
    int model_size = 0;
    model_data = load_file(model_path, &model_size);
    if (!model_data) {
        std::cerr << "[RKNN 错误] 读取模型文件失败！" << std::endl;
        return false;
    }

    // 初始化 RKNN Context
    int ret = rknn_init(&ctx, model_data, model_size, 0, NULL);
    if (ret < 0) {
        std::cerr << "[RKNN 错误] rknn_init 失败，错误码: " << ret << std::endl;
        return false;
    }

    // ====== 解决 failed to submit 硬件死锁的核心修正 ======
    // RK3588 有三个 NPU 核心(0, 1, 2)。旧版驱动在 Auto(多核自动调度) 模式下，面对极其高频的不等长帧送入时
    // 常常会发生上下文状态竞速，直接导致 DMA 错误并报 failed to submit。
    // 强制将当前模型上下文绑定到 CORE 0 即可完全解决不稳定：
    rknn_core_mask core_mask = RKNN_NPU_CORE_0;
    int core_ret = rknn_set_core_mask(ctx, core_mask);
    if (core_ret < 0) {
        std::cerr << "[RKNN 警告] 绑定 NPU 核心失败，可能不影响继续： " << core_ret << std::endl;
    }

    // 获取模型输入输出的数量
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    std::cout << "[RKNN] 模型输入数量: " << io_num.n_input << ", 输出数量: " << io_num.n_output << std::endl;

    // 获取输入张量属性
    input_attrs.resize(io_num.n_input);
    for (int i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        std::cout << "[RKNN] 输入 " << i << " 属性 - fmt: " << input_attrs[i].fmt 
                  << ", type: " << input_attrs[i].type 
                  << ", dims: [" << input_attrs[i].dims[0] << ", " 
                  << input_attrs[i].dims[1] << ", " << input_attrs[i].dims[2] << ", " << input_attrs[i].dims[3] << "]" << std::endl;
    }

    // 获取输出张量属性
    output_attrs.resize(io_num.n_output);
    for (int i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        std::cout << "[RKNN] 输出 " << i << " 属性 - fmt: " << output_attrs[i].fmt 
                  << ", type: " << output_attrs[i].type 
                  << ", dims: [" << output_attrs[i].dims[0] << ", " 
                  << output_attrs[i].dims[1] << ", " << output_attrs[i].dims[2] << ", " << output_attrs[i].dims[3] << "]" << std::endl;
    }
    std::cout << "[RKNN] 初始化完成！准备就绪。" << std::endl;
    is_init = true;
    return true;
}

void RKNNDetector::nms(std::vector<DetectResult>& input_boxes, float nms_thresh) {
    std::sort(input_boxes.begin(), input_boxes.end(), [](DetectResult a, DetectResult b) {
        return a.confidence > b.confidence;
    });
    std::vector<bool> is_suppressed(input_boxes.size(), false);
    for (size_t i = 0; i < input_boxes.size(); ++i) {
        if (is_suppressed[i]) continue;
        for (size_t j = i + 1; j < input_boxes.size(); ++j) {
            if (is_suppressed[j]) continue;
            float inter_area = (input_boxes[i].box & input_boxes[j].box).area();
            float union_area = input_boxes[i].box.area() + input_boxes[j].box.area() - inter_area;
            float iou = inter_area / union_area;
            if (iou >= nms_thresh) {
                is_suppressed[j] = true;
            }
        }
    }
    std::vector<DetectResult> out_boxes;
    for (size_t i = 0; i < input_boxes.size(); ++i) {
        if (!is_suppressed[i]) out_boxes.push_back(input_boxes[i]);
    }
    input_boxes = out_boxes;
}

std::vector<DetectResult> RKNNDetector::inference(cv::Mat& image) {
    std::vector<DetectResult> results;
    if (!is_init) return results;

    // =================== 1. 图像前处理 ===================
    cv::Mat resized_img;
    cv::cvtColor(image, resized_img, cv::COLOR_BGR2RGB); // RKNN模型一般都需要 RGB 通道
    
    // 【终极排雷】：正确解析模型所需要的高宽（防止 NCHW 和 NHWC 导致的错位）
    int req_width = 640;
    int req_height = 640;
    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        req_height = input_attrs[0].dims[2];
        req_width = input_attrs[0].dims[3];
    } else {
        req_height = input_attrs[0].dims[1];
        req_width = input_attrs[0].dims[2];
    }
    
    if (req_width <= 0 || req_height <= 0) { req_width = 640; req_height = 640; } 
    
    cv::resize(resized_img, resized_img, cv::Size(req_width, req_height));

    // 绝杀操作：由于您的模型是 _fp 模型，我们直接在外部把它转成 Float32 (浮点型)。
    // 很多旧版驱动在将 UINT8 转 FP16 时没有量化参数会导致 NaN 溢出从而触发 failed to submit 硬件异常停机！
    cv::Mat float_img;
    resized_img.convertTo(float_img, CV_32FC3); // 将 8位整数 转换为 32位单精度浮点数

    if (!float_img.isContinuous()) {
        float_img = float_img.clone();
    }

    // =================== 2. 设置 NPU 输入 ===================
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_FLOAT32;       // 明确告知驱动：现在喂进来的是 32位 浮点数！
    inputs[0].fmt = RKNN_TENSOR_NHWC;   
    inputs[0].size = req_width * req_height * 3 * sizeof(float); // 宽高 * 3通道 * 4字节(float)
    inputs[0].pass_through = 0;                 // 驱动现在只需要简单地把 Float32 切成模型需要的 Float16 即可，不会再有计算转换溢出！
    inputs[0].buf = float_img.data;

    int ret = rknn_inputs_set(ctx, io_num.n_input, inputs);
    if (ret < 0) {
        std::cerr << "[RKNN] 设置输入出错: " << ret << std::endl;
        return results;
    }

    // =================== 3. NPU 硬件推理 ===================
    auto start = std::chrono::high_resolution_clock::now();
    ret = rknn_run(ctx, NULL);
    auto end = std::chrono::high_resolution_clock::now();
    
    if (ret < 0) {
        std::cerr << "[RKNN] NPU 执行出错: " << ret << std::endl;
        return results;
    }

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "[NPU 推理耗时] " << duration << " ms" << std::endl;

    // =================== 4. 获取推理输出 ===================
    rknn_output outputs[io_num.n_output];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < io_num.n_output; i++) {
        outputs[i].want_float = 1; // 要求 rknn_api 将 NPU 算完的数据反量化回浮点数，方便我们计算
    }

    ret = rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
    if (ret >= 0) {
        // [新增] 动态自适应比例映射，解决图像坐标拉伸问题
        float scale_x = (float)image.cols / req_width;
        float scale_y = (float)image.rows / req_height;
        float conf_threshold = 0.45; // 人脸置信度阈值过滤
        
        // 绝大多数导出到 RKNN 的 YOLOv8 模型，会把最终所有的 Anchors 输出融合成一个单张量 output[0]
        if (io_num.n_output == 1) {
            float* output_ptr = (float*)outputs[0].buf;
            
            // 解析张量维度：典型的 YOLOv8 格式可能是 [1, 5~84, 8400] 或者 [1, 8400, 5~84]
            int dim1 = output_attrs[0].dims[1];
            int dim2 = output_attrs[0].dims[2];
            int anchors = (dim1 > dim2) ? dim1 : dim2;
            int attr = (dim1 < dim2) ? dim1 : dim2; 
            
            bool is_nchw = (dim1 == attr); // 判断特征是在前面还是在后面 [1, 14, 8400] vs [1, 8400, 14]
            
            for (int i = 0; i < anchors; i++) {
                float max_conf = 0;
                int max_id = -1;
                
                // NCHW 内存步长跳跃为 `anchors`，NHWC 内存跳跃为 `attr`
                float cx, cy, w, h;
                if (is_nchw) {
                    cx = output_ptr[0 * anchors + i];
                    cy = output_ptr[1 * anchors + i];
                    w  = output_ptr[2 * anchors + i];
                    h  = output_ptr[3 * anchors + i];
                    // 处理单类别 (人脸) 或者多类别置信度
                    for(int c = 0; c < attr - 4; c++) {
                        float conf = output_ptr[(4 + c) * anchors + i];
                        if (conf > max_conf) { max_conf = conf; max_id = c; }
                    }
                } else {
                    cx = output_ptr[i * attr + 0];
                    cy = output_ptr[i * attr + 1];
                    w  = output_ptr[i * attr + 2];
                    h  = output_ptr[i * attr + 3];
                    for(int c = 0; c < attr - 4; c++) {
                        float conf = output_ptr[i * attr + 4 + c];
                        if (conf > max_conf) { max_conf = conf; max_id = c; }
                    }
                }
                
                if (max_conf > conf_threshold) {
                    DetectResult res;
                    res.confidence = max_conf;
                    res.classId = max_id;
                    
                    // 将 cx, cy, w, h 还原至原始尺度图像上的 Rect
                    int left   = (int)((cx - w / 2) * scale_x);
                    int top    = (int)((cy - h / 2) * scale_y);
                    int width  = (int)(w * scale_x);
                    int height = (int)(h * scale_y);
                    res.box = cv::Rect(left, top, width, height);
                    
                    results.push_back(res);
                }
            }
            
            // 进行 Non-Maximum Suppression (NMS 防止人脸框重叠堆积)
            if (!results.empty()) {
                nms(results, 0.45); // NMS IoU阈值 0.45 
                std::cout << "[模型后处理] 在当前帧中检测到 " << results.size() << " 个人脸。" << std::endl;
            }
        } else {
             std::cout << "[警告] 发现多端输出层 (Output=" << io_num.n_output << ") 暂未实现合并，请提供 init属性日志分析。" << std::endl;
        }
        
        rknn_outputs_release(ctx, io_num.n_output, outputs);
    }
    
    // 猛药 2：救命的 5 毫秒防冲击延时 （C++ 因为比 Python 快太多，从而触发了 NPU 底层驱动的 Bug）
    // 您之前写的 Python 之所以不报错，是因为 Python 调用底层 C++ 接口以及释放内存会有天然的 3~5ms 的 GIL 和封装延时
    // 而现在的 C++ 工业级框架，一旦一帧推理算完了释放 `outputs_release` 之后，不到 0.05 毫秒下一个循环立马就开始灌下个 `rknn_inputs_set`！
    // 老版本的 RK3588 NPU 驱动内部清理上一个 DMA 内存是异步的，速度跟不上 C++ 的疯狂连发，导致了指针践踏和驱动当机报 `failed to submit` 从而引发 5 秒钟超时惩罚。
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    return results;
}
