#include "rga_yuv_converter.h"
#include <opencv2/opencv.hpp>

#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unordered_map>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#define RK_HAS_DMA_HEAP_HEADER 1
#else
#define RK_HAS_DMA_HEAP_HEADER 0
#endif

#include "im2d_version.h"
#include "im2d_buffer.h"
#include "im2d_common.h"
#include "im2d_single.h"
#include "im2d_type.h"
#include "rga.h"

namespace {

class Dma32RgaBuffer {
public:
    ~Dma32RgaBuffer() {
        release();
    }

    bool ensure(size_t requested_size, const std::string& heap_path) {
#if RK_HAS_DMA_HEAP_HEADER
        if (requested_size == 0) {
            return false;
        }
        if (mapped_addr_ != MAP_FAILED && size_ >= requested_size) {
            return true;
        }
        release();

        heap_fd_ = ::open(heap_path.c_str(), O_RDWR | O_CLOEXEC);
        if (heap_fd_ < 0) {
            return false;
        }

        dma_heap_allocation_data alloc_data{};
        alloc_data.len = requested_size;
        alloc_data.fd_flags = O_RDWR | O_CLOEXEC;
        alloc_data.heap_flags = 0;
        if (ioctl(heap_fd_, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0) {
            release();
            return false;
        }

        buffer_fd_ = alloc_data.fd;
        mapped_addr_ = mmap(nullptr, requested_size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer_fd_, 0);
        if (mapped_addr_ == MAP_FAILED) {
            release();
            return false;
        }

        size_ = requested_size;
        handle_ = importbuffer_fd(buffer_fd_, static_cast<int>(requested_size));
        if (handle_ == 0) {
            release();
            return false;
        }
        return true;
#else
        (void)requested_size;
        (void)heap_path;
        return false;
#endif
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
        if (buffer_fd_ >= 0) {
            ::close(buffer_fd_);
            buffer_fd_ = -1;
        }
        if (heap_fd_ >= 0) {
            ::close(heap_fd_);
            heap_fd_ = -1;
        }
        size_ = 0;
    }

    void* addr() const { return mapped_addr_; }
    rga_buffer_handle_t handle() const { return handle_; }

private:
    int heap_fd_ = -1;
    int buffer_fd_ = -1;
    void* mapped_addr_ = MAP_FAILED;
    size_t size_ = 0;
    rga_buffer_handle_t handle_ = 0;
};

struct RgaYuvRuntimeCache {
    Dma32RgaBuffer src_dma;
    Dma32RgaBuffer dst_dma;
};

RgaYuvRuntimeCache& cacheForYuvContext(RgaYuvConverterContext* context) {
    static std::unordered_map<RgaYuvConverterContext*, RgaYuvRuntimeCache> caches;
    return caches[context];
}

} // namespace

void initializeRgaYuvConverterContext(int stream_id,
                                      bool use_rga_conversion,
                                      bool use_dma32_for_rga,
                                      int rga_scheduler_mode,
                                      RgaYuvConverterContext& context) {
    context.use_rga_conversion = use_rga_conversion;
    context.current_use_dma32 = true;

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

    IM_STATUS cfg_ret = imconfig(IM_CONFIG_SCHEDULER_CORE, scheduler_core);
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

    IM_STATUS cfg_runtime_ret = imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0);
    if (cfg_runtime_ret != IM_STATUS_SUCCESS && cfg_runtime_ret != IM_STATUS_NOERROR) {
        context.conversion_fused_off = true;
        std::cerr << "[RGA-YUV] 运行时调度配置失败，熔断到 CPU。错误: "
                  << imStrError(cfg_runtime_ret) << std::endl;
        return false;
    }

    RgaYuvRuntimeCache& runtime_cache = cacheForYuvContext(&context);
    IM_STATUS rga_status = IM_STATUS_FAILED;

    // 计算源数据大小 (YUV422: 每像素2字节)
    const size_t src_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
    // 目标数据大小 (BGR888: 每像素3字节)
    const size_t dst_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;

    bool src_ok = runtime_cache.src_dma.ensure(src_bytes, dma_heap_path);
    bool dst_ok = runtime_cache.dst_dma.ensure(dst_bytes, dma_heap_path);
    
    if (src_ok && dst_ok) {
        // 复制源数据到DMA缓冲区
        unsigned char* src_ptr = reinterpret_cast<unsigned char*>(runtime_cache.src_dma.addr());
        unsigned char* src_original = reinterpret_cast<unsigned char*>(src_buffer);
        
        // 如果bytes_per_line与width*2不同，则需要逐行复制
        if (static_cast<int>(width * 2) == bytes_per_line) {
            // 数据连续，直接复制整个块
            std::memcpy(src_ptr, src_original, src_bytes);
        } else {
            // 逐行复制，处理padding
            for (int y = 0; y < height; ++y) {
                std::memcpy(src_ptr + static_cast<size_t>(y) * static_cast<size_t>(width) * 2,
                            src_original + static_cast<size_t>(y) * static_cast<size_t>(bytes_per_line),
                            static_cast<size_t>(width) * 2);
            }
        }

        // 创建RGA缓冲区描述符
        rga_buffer_t src_img = wrapbuffer_handle(runtime_cache.src_dma.handle(),
                                                 width,
                                                 height,
                                                 RK_FORMAT_YUYV_422,
                                                 width * 2,  // stride
                                                 height);

        rga_buffer_t dst_img = wrapbuffer_handle(runtime_cache.dst_dma.handle(),
                                                 width,
                                                 height,
                                                 RK_FORMAT_BGR_888,
                                                 width * 3,  // stride
                                                 height);

        // 执行YUV到BGR的转换
        rga_status = imcvtcolor(src_img, dst_img, RK_FORMAT_YUYV_422, RK_FORMAT_BGR_888);
        
        if (rga_status == IM_STATUS_SUCCESS) {
            // 创建OpenCV Mat视图，不需要额外内存分配
            dst_bgr_frame = cv::Mat(height, width, CV_8UC3, runtime_cache.dst_dma.addr(), width * 3);
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

    IM_STATUS cfg_runtime_ret = imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0);
    if (cfg_runtime_ret != IM_STATUS_SUCCESS && cfg_runtime_ret != IM_STATUS_NOERROR) {
        context.conversion_fused_off = true;
        std::cerr << "[RGA-YUV] 运行时调度配置失败，熔断到 CPU。错误: "
                  << imStrError(cfg_runtime_ret) << std::endl;
        return false;
    }

    RgaYuvRuntimeCache& runtime_cache = cacheForYuvContext(&context);
    IM_STATUS rga_status = IM_STATUS_FAILED;

    // 计算YUV420数据大小 (Y分量 + UV分量 = width*height + width*height/2)
    const size_t y_size = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t uv_size = y_size / 2; // U和V各占一半
    const size_t src_bytes = y_size + uv_size;
    // 目标数据大小 (BGR888: 每像素3字节)
    const size_t dst_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;

    bool src_ok = runtime_cache.src_dma.ensure(src_bytes, dma_heap_path);
    bool dst_ok = runtime_cache.dst_dma.ensure(dst_bytes, dma_heap_path);
    
    if (src_ok && dst_ok) {
        // 复制源数据到DMA缓冲区
        unsigned char* src_ptr = reinterpret_cast<unsigned char*>(runtime_cache.src_dma.addr());
        unsigned char* src_original = reinterpret_cast<unsigned char*>(src_buffer);
        
        std::memcpy(src_ptr, src_original, src_bytes);

        // 创建RGA缓冲区描述符 (NV12格式，YUV420的一种)
        rga_buffer_t src_img = wrapbuffer_handle(runtime_cache.src_dma.handle(),
                                                 width,
                                                 height,
                                                 RK_FORMAT_YCbCr_420_SP,  // NV12格式
                                                 width,                   // stride
                                                 height);

        rga_buffer_t dst_img = wrapbuffer_handle(runtime_cache.dst_dma.handle(),
                                                 width,
                                                 height,
                                                 RK_FORMAT_BGR_888,
                                                 width * 3,  // stride
                                                 height);

        // 执行YUV到BGR的转换
        rga_status = imcvtcolor(src_img, dst_img, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888);
        
        if (rga_status == IM_STATUS_SUCCESS) {
            // 创建OpenCV Mat视图，不需要额外内存分配
            dst_bgr_frame = cv::Mat(height, width, CV_8UC3, runtime_cache.dst_dma.addr(), width * 3);
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