#pragma once

#include <cstdint>
#include <string>

// 包含OpenCV头文件以正确处理cv::Mat类型
#ifdef __cplusplus
#include <opencv2/opencv.hpp>
#endif

struct RgaYuvConverterContext {
    bool use_rga_conversion = true;
    bool conversion_fused_off = false;
    bool current_use_dma32 = true;
    uint64_t scheduler_core = 0;
};

/**
 * 初始化RGA YUV转换器上下文
 */
void initializeRgaYuvConverterContext(int stream_id,
                                      bool use_rga_conversion,
                                      bool use_dma32_for_rga,
                                      int rga_scheduler_mode,
                                      RgaYuvConverterContext& context);

/**
 * 尝试使用RGA进行YUV到BGR的转换
 * @param src_buffer 源YUV数据缓冲区
 * @param width 源图像宽度
 * @param height 源图像高度
 * @param bytes_per_line 源图像每行字节数
 * @param dst_bgr_frame 目标BGR Mat对象
 * @param dma_heap_path DMA堆路径
 * @param context RGA转换上下文
 * @return 是否成功使用RGA转换
 */
bool tryConvertYuvToBgrWithRga(void* src_buffer,
                               int width,
                               int height,
                               int bytes_per_line,
                               cv::Mat& dst_bgr_frame,
                               const std::string& dma_heap_path,
                               RgaYuvConverterContext& context);

/**
 * 使用RGA进行YUV422到BGR888的转换
 */
bool convertYuv422ToBgrWithRga(void* src_buffer,
                                int width,
                                int height,
                                int bytes_per_line,
                                cv::Mat& dst_bgr_frame,
                                const std::string& dma_heap_path,
                                RgaYuvConverterContext& context);

/**
 * 使用RGA进行YUV420到BGR888的转换
 */
bool convertYuv420ToBgrWithRga(void* src_buffer,
                                int width,
                                int height,
                                cv::Mat& dst_bgr_frame,
                                const std::string& dma_heap_path,
                                RgaYuvConverterContext& context);
