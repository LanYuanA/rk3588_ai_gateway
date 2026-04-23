#include "rga_yuv_converter.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// #if __has_include(<linux/dma-heap.h>)
// #include <linux/dma-heap.h>
// #define RK_HAS_DMA_HEAP_HEADER 1
// #else
// #define RK_HAS_DMA_HEAP_HEADER 0
// #endif

#include "im2d_version.h"
#include "im2d_buffer.h"
#include "im2d_common.h"
#include "im2d_single.h"
#include "im2d_type.h"
#include "rga.h"

namespace {

/**
 * \brief 使用普通内存的RGA缓冲区类，仅使用RGA3核心
 */
class RgaBuffer {
public:
    ~RgaBuffer() {
        release();
    }

    /**
     * \brief 确保分配指定大小的内存
     * \param requested_size 请求的内存大小
     * \return 成功返回true，失败返回false
     * 
     * 注意：此函数使用普通内存分配，仅适用于RGA3核心
     */
    bool ensure(size_t requested_size) {
        if (requested_size == 0) {
            return false;
        }
        if (mapped_addr_ != MAP_FAILED && size_ >= requested_size) {
            return true;
        }
        release();

        // 使用mmap分配内存，而不是DMA heap
        mapped_addr_ = mmap(nullptr, requested_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped_addr_ == MAP_FAILED) {
            return false;
        }

        size_ = requested_size;
        // 将分配的虚拟内存导入RGA驱动，使其能够被RGA硬件访问
        handle_ = importbuffer_virtualaddr(mapped_addr_, static_cast<int>(requested_size));
        if (handle_ == 0) {
            release();
            return false;
        }
        return true;
    }

    void release() {
        if (handle_ != 0) {
            releasebuffer_handle(handle_);
            handle_ = 0;
        }
        if (mapped_addr_ != MAP_FAILED) {
            munmap(mapped_addr_, size_);
            mapped_addr_ = MAP_FAILED;
        }
        size_ = 0;
    }

    void* addr() const { return mapped_addr_; }
    rga_buffer_handle_t handle() const { return handle_; }

private:
    void* mapped_addr_ = MAP_FAILED;
    size_t size_ = 0;
    rga_buffer_handle_t handle_ = 0;
};

struct RgaYuvRuntimeCache {
    RgaBuffer src_buffer;
    RgaBuffer dst_buffer;
};

RgaYuvRuntimeCache& cacheForYuvContext(RgaYuvConverterContext* context) {
    thread_local std::unordered_map<RgaYuvConverterContext*, RgaYuvRuntimeCache> caches;
    return caches[context];
}

int alignUp(int value, int alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

} // namespace

void initializeRgaYuvConverterContext(int stream_id,
                                      bool use_rga_conversion,
                                      bool use_dma32_for_rga,
                                      int rga_scheduler_mode,
                                      RgaYuvConverterContext& context) {
    context.use_rga_conversion = use_rga_conversion;
    context.current_use_dma32 = true;
    context.conversion_fused_off = false;

    if (!use_dma32_for_rga) {
        std::cerr << "[RGA-YUV] RK_RGA_USE_DMA32=0 已被忽略：当前版本强制 DMA32 模式以避免 >4G 风险。"
                  << std::endl;
    }

    if (!context.use_rga_conversion) {
        return;
    }

    // 默认使用RGA3核心0进行YUV转换
    uint64_t scheduler_core = IM_SCHEDULER_RGA3_CORE0;
    
    if (rga_scheduler_mode == 3) {
        scheduler_core = IM_SCHEDULER_RGA2_CORE0;
    } else if (rga_scheduler_mode == 0) {
        scheduler_core = IM_SCHEDULER_RGA3_CORE0;
    } else if (rga_scheduler_mode == 1) {
        scheduler_core = IM_SCHEDULER_RGA3_CORE1;
    } else if (rga_scheduler_mode == 2) {
        scheduler_core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1;
    } else {
        scheduler_core = (stream_id % 2 == 0) ? IM_SCHEDULER_RGA3_CORE0 : IM_SCHEDULER_RGA3_CORE1;
    }

    context.scheduler_core = scheduler_core;
    IM_STATUS cfg_ret = imconfig(IM_CONFIG_SCHEDULER_CORE, context.scheduler_core);
    if (cfg_ret != IM_STATUS_SUCCESS && cfg_ret != IM_STATUS_NOERROR) {
        std::cerr << "[RGA-YUV] stream " << stream_id
                  << " 调度核心配置失败，将回退 CPU 转换。错误: "
                  << imStrError(cfg_ret) << std::endl;
        context.conversion_fused_off = true;
    } else {
        std::cout << "[RGA-YUV] stream " << stream_id
                  << " 调度到 RGA 核心: 0x" << std::hex << scheduler_core << std::dec << std::endl;
    }
}

bool tryConvertYuvToBgrWithRga(void* src_buffer,
                               int width,
                               int height,
                               int bytes_per_line,
                               cv::Mat& dst_bgr_frame,
                               const std::string& dma_heap_path,
                               RgaYuvConverterContext& context) {
    if (!context.use_rga_conversion || context.conversion_fused_off) {
        return false;
    }

    // 尝试使用YUV422格式转换
    return convertYuv422ToBgrWithRga(src_buffer, width, height, bytes_per_line, dst_bgr_frame, dma_heap_path, context);
}

bool convertYuv422ToBgrWithRga(void* src_buffer,
                               int width,
                               int height,
                               int bytes_per_line,
                               cv::Mat& dst_bgr_frame,
                               const std::string& dma_heap_path,
                               RgaYuvConverterContext& context) {
    if (!context.use_rga_conversion || context.conversion_fused_off) {
        return false;
    }

    IM_STATUS cfg_runtime_ret = imconfig(IM_CONFIG_SCHEDULER_CORE, context.scheduler_core);
    if (cfg_runtime_ret != IM_STATUS_SUCCESS && cfg_runtime_ret != IM_STATUS_NOERROR) {
        context.conversion_fused_off = true;
        std::cerr << "[RGA-YUV] 运行时调度配置失败，熔断到 CPU。错误: "
                  << imStrError(cfg_runtime_ret) << std::endl;
        return false;
    }

    RgaYuvRuntimeCache& runtime_cache = cacheForYuvContext(&context);
    IM_STATUS rga_status = IM_STATUS_FAILED;

    // 根据官方示例，使用正确的stride计算方式
    const int src_bpp = 2; // YUV422每个像素2字节
    const int dst_bpp = 3; // BGR每个像素3字节
    const int src_stride = width * src_bpp;  // 官方示例中使用实际像素宽度*字节数
    const int dst_stride = width * dst_bpp;  // 官方示例中使用实际像素宽度*字节数
    const size_t src_bytes = static_cast<size_t>(src_stride) * static_cast<size_t>(height);
    const size_t dst_bytes = static_cast<size_t>(dst_stride) * static_cast<size_t>(height);

    // 为RGA可能的内部对齐需求预留额外空间
    const size_t src_padding = 4096; // 预留4KB对齐空间
    const size_t dst_padding = 4096; // 预留4KB对齐空间
    const size_t aligned_src_bytes = src_bytes + src_padding;
    const size_t aligned_dst_bytes = dst_bytes + dst_padding;

    bool src_ok = runtime_cache.src_buffer.ensure(aligned_src_bytes);
    bool dst_ok = runtime_cache.dst_buffer.ensure(aligned_dst_bytes);
    
    if (src_ok && dst_ok) {
        // 复制源数据到缓冲区
        unsigned char* src_ptr = reinterpret_cast<unsigned char*>(runtime_cache.src_buffer.addr());
        unsigned char* src_original = reinterpret_cast<unsigned char*>(src_buffer);
        std::memset(src_ptr, 0, aligned_src_bytes);  // 清零整个缓冲区

        // 按照官方示例的方式复制数据
        const int copy_bytes = std::min(bytes_per_line, src_stride);
        for (int y = 0; y < height; ++y) {
            std::memcpy(src_ptr + static_cast<size_t>(y) * static_cast<size_t>(src_stride),
                        src_original + static_cast<size_t>(y) * static_cast<size_t>(bytes_per_line),
                        static_cast<size_t>(copy_bytes));
        }

        // 使用参考代码的方法，直接使用wrapbuffer_virtualaddr
        rga_buffer_t src_img = wrapbuffer_virtualaddr(src_ptr, width, height, RK_FORMAT_YUYV_422);
        rga_buffer_t dst_img = wrapbuffer_virtualaddr(runtime_cache.dst_buffer.addr(), width, height, RK_FORMAT_BGR_888);

        // 设置源和目标矩形区域
        im_rect src_rect = {0, 0, width, height};
        im_rect dst_rect = {0, 0, width, height};

        // 按照官方示例进行参数检查
        IM_STATUS check_ret = imcheck(src_img, dst_img, src_rect, dst_rect);
        if (check_ret != IM_STATUS_NOERROR && check_ret != IM_STATUS_SUCCESS) {
            std::cerr << "[RGA-YUV] 422 参数预检失败: " << imStrError(check_ret)
                      << " width=" << width
                      << " height=" << height
                      << " bytes_per_line=" << bytes_per_line
                      << " src_stride=" << src_stride
                      << " dst_stride=" << dst_stride
                      << std::endl;
            rga_status = check_ret;
            context.conversion_fused_off = true;
            return false;
        }

        // 按照官方示例执行YUV到BGR的转换
        rga_status = imcvtcolor(src_img, dst_img, RK_FORMAT_YUYV_422, RK_FORMAT_BGR_888);
        
        if (rga_status == IM_STATUS_SUCCESS) {
            // 创建临时OpenCV Mat并复制到目标Mat
            cv::Mat temp_mat(height, width, CV_8UC3, runtime_cache.dst_buffer.addr(), dst_stride);
            temp_mat.copyTo(dst_bgr_frame);  // 复制到目标Mat，避免生命周期问题
            return true;
        }
    } else {
        rga_status = IM_STATUS_OUT_OF_MEMORY;
    }

    context.conversion_fused_off = true;
    std::cerr << "[RGA-YUV] YUV422到BGR转换失败，已自动熔断为 CPU 转换。错误: "
              << imStrError(rga_status) << " (" << static_cast<int>(rga_status) << ")"
              << std::endl;
    return false;
}

bool convertYuv420ToBgrWithRga(void* src_buffer,
                               int width,
                               int height,
                               cv::Mat& dst_bgr_frame,
                               const std::string& dma_heap_path,
                               RgaYuvConverterContext& context) {
    if (!context.use_rga_conversion || context.conversion_fused_off) {
        return false;
    }

    IM_STATUS cfg_runtime_ret = imconfig(IM_CONFIG_SCHEDULER_CORE, context.scheduler_core);
    if (cfg_runtime_ret != IM_STATUS_SUCCESS && cfg_runtime_ret != IM_STATUS_NOERROR) {
        context.conversion_fused_off = true;
        std::cerr << "[RGA-YUV] 运行时调度配置失败，熔断到 CPU。错误: "
                  << imStrError(cfg_runtime_ret) << std::endl;
        return false;
    }

    RgaYuvRuntimeCache& runtime_cache = cacheForYuvContext(&context);
    IM_STATUS rga_status = IM_STATUS_FAILED;

    // 计算NV12格式的正确大小
    const int y_size = width * height;
    const int uv_size = width * height / 2; // UV交错存储
    const int src_total_size = y_size + uv_size; // NV12总大小
    const int dst_bpp = 3; // BGR每个像素3字节
    const int dst_stride = width * dst_bpp;
    const size_t dst_bytes = static_cast<size_t>(dst_stride) * static_cast<size_t>(height);

    // 为RGA可能的内部对齐需求预留额外空间
    const size_t src_padding = 4096; // 预留4KB对齐空间
    const size_t dst_padding = 4096; // 预留4KB对齐空间
    const size_t aligned_src_bytes = src_total_size + src_padding;
    const size_t aligned_dst_bytes = dst_bytes + dst_padding;

    bool src_ok = runtime_cache.src_buffer.ensure(aligned_src_bytes);
    bool dst_ok = runtime_cache.dst_buffer.ensure(aligned_dst_bytes);
    
    if (src_ok && dst_ok) {
        // 复制源数据到缓冲区 - 直接复制整个NV12缓冲区
        unsigned char* src_ptr = reinterpret_cast<unsigned char*>(runtime_cache.src_buffer.addr());
        unsigned char* src_original = reinterpret_cast<unsigned char*>(src_buffer);
        
        std::memset(src_ptr, 0, aligned_src_bytes);
        std::memcpy(src_ptr, src_original, src_total_size); // 复制完整的NV12数据

        // 使用参考代码的方法，直接使用wrapbuffer_virtualaddr
        unsigned char* src_ptr_nv12 = reinterpret_cast<unsigned char*>(runtime_cache.src_buffer.addr());
        unsigned char* dst_ptr = reinterpret_cast<unsigned char*>(runtime_cache.dst_buffer.addr());
        
        rga_buffer_t src_img = wrapbuffer_virtualaddr(src_ptr_nv12, width, height, RK_FORMAT_YCbCr_420_SP);
        rga_buffer_t dst_img = wrapbuffer_virtualaddr(dst_ptr, width, height, RK_FORMAT_BGR_888);

        // 设置源和目标矩形区域
        im_rect src_rect = {0, 0, width, height};
        im_rect dst_rect = {0, 0, width, height};

        // 按照官方示例进行参数检查
        IM_STATUS check_ret = imcheck(src_img, dst_img, src_rect, dst_rect);
        if (check_ret != IM_STATUS_NOERROR && check_ret != IM_STATUS_SUCCESS) {
            std::cerr << "[RGA-YUV] 420 参数预检失败: " << imStrError(check_ret)
                      << " width=" << width
                      << " height=" << height
                      << std::endl;
            rga_status = check_ret;
            context.conversion_fused_off = true;
            return false;
        }

        // 执行YUV到BGR的转换
        rga_status = imcvtcolor(src_img, dst_img, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888);
        
        if (rga_status == IM_STATUS_SUCCESS) {
            // 创建临时OpenCV Mat并复制到目标Mat以避免生命周期问题
            cv::Mat temp_mat(height, width, CV_8UC3, runtime_cache.dst_buffer.addr(), dst_stride);
            temp_mat.copyTo(dst_bgr_frame);  // 复制到目标Mat，避免生命周期问题
            return true;
        }
    } else {
        rga_status = IM_STATUS_OUT_OF_MEMORY;
    }

    context.conversion_fused_off = true;
    std::cerr << "[RGA-YUV] YUV420到BGR转换失败，已自动熔断为 CPU 转换。错误: "
              << imStrError(rga_status) << " (" << static_cast<int>(rga_status) << ")"
              << std::endl;
    return false;
}
