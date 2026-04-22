#pragma once

#include <cstdint>
#include <opencv2/opencv.hpp>
#include <string>

struct RgaResizeContext {
    bool use_rga_resize = true;
    bool current_use_dma32 = true;
    bool rga_resize_fused_off = false;
    bool use_rga2_mode = false;
    uint64_t scheduler_core = 0;
};

void initializeRgaResizeContext(int stream_id,
                                bool use_rga_resize,
                                bool use_dma32_for_rga,
                                int rga_scheduler_mode,
                                RgaResizeContext& context);

bool tryResizeTo640x480WithRga(cv::Mat& bgr_frame,
                               const std::string& dma_heap_path,
                               RgaResizeContext& context);
