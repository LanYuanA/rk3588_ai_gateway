#include "rga_resize.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <iostream>
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

struct RgaRuntimeCache {
    RgaBuffer src_buffer;
    RgaBuffer dst_buffer;
};

RgaRuntimeCache& cacheForContext(RgaResizeContext* context) {
    thread_local std::unordered_map<RgaResizeContext*, RgaRuntimeCache> caches;
    return caches[context];
}

int alignUp(int value, int alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

} // namespace

void initializeRgaResizeContext(int stream_id,
                                bool use_rga_resize,
                                bool use_dma32_for_rga,
                                int rga_scheduler_mode,
                                RgaResizeContext& context) {
    context.use_rga_resize = use_rga_resize;
    context.current_use_dma32 = true;
    context.rga_resize_fused_off = false;
    context.use_rga2_mode = false;
    context.scheduler_core = IM_SCHEDULER_RGA3_CORE0;

    if (!use_dma32_for_rga) {
        std::cerr << "[RGA] RK_RGA_USE_DMA32=0 已被忽略：当前版本强制 DMA32 模式以避免 >4G 风险。"
                  << std::endl;
    }

    if (!context.use_rga_resize) {
        return;
    }

    if (rga_scheduler_mode == 3) {
        context.scheduler_core = IM_SCHEDULER_RGA2_CORE0;
        context.use_rga2_mode = true;
    } else if (rga_scheduler_mode == 0) {
        context.scheduler_core = IM_SCHEDULER_RGA3_CORE0;
    } else if (rga_scheduler_mode == 1) {
        context.scheduler_core = IM_SCHEDULER_RGA3_CORE1;
    } else if (rga_scheduler_mode == 2) {
        context.scheduler_core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1;
    } else {
        context.scheduler_core = (stream_id % 2 == 0) ? IM_SCHEDULER_RGA3_CORE0 : IM_SCHEDULER_RGA3_CORE1;
    }

    IM_STATUS cfg_ret = imconfig(IM_CONFIG_SCHEDULER_CORE, context.scheduler_core);
    if (cfg_ret != IM_STATUS_SUCCESS && cfg_ret != IM_STATUS_NOERROR) {
        std::cerr << "[RGA] stream " << stream_id
                  << " 调度核心配置失败，将回退 CPU 缩放。错误: "
                  << imStrError(cfg_ret) << std::endl;
        context.rga_resize_fused_off = true;
    } else {
        std::cout << "[RGA] stream " << stream_id;
        if (context.use_rga2_mode) {
            std::cout << " 调度到 RGA2_CORE0 (demo DMA32 模式强制开启)";
        } else {
            std::cout << " 调度到 RGA3 核掩码: 0x" << std::hex << context.scheduler_core << std::dec;
        }
        std::cout << std::endl;
    }
}

bool tryResizeTo640x480WithRga(cv::Mat& bgr_frame,
                               const std::string& dma_heap_path,
                               RgaResizeContext& context) {
    if (!context.use_rga_resize || context.rga_resize_fused_off) {
        return false;
    }
    if (bgr_frame.cols == 640 && bgr_frame.rows == 480) {
        return true;
    }

    IM_STATUS cfg_runtime_ret = imconfig(IM_CONFIG_SCHEDULER_CORE, context.scheduler_core);
    if (cfg_runtime_ret != IM_STATUS_SUCCESS && cfg_runtime_ret != IM_STATUS_NOERROR) {
        context.rga_resize_fused_off = true;
        std::cerr << "[RGA] 运行时调度配置失败，熔断到 CPU。错误: "
                  << imStrError(cfg_runtime_ret) << std::endl;
        return false;
    }

    RgaRuntimeCache& runtime_cache = cacheForContext(&context);
    IM_STATUS rga_status = IM_STATUS_FAILED;

    const int src_wstride = std::max(bgr_frame.cols, alignUp(static_cast<int>((bgr_frame.step + 2) / 3), 16));
    const int dst_wstride = alignUp(640, 16);
     // 根据官方示例，使用实际的stride值
     const int actual_src_stride = bgr_frame.cols * 3;  // 每个像素3字节(BGR)
     const int actual_dst_stride = 640 * 3;             // 每个像素3字节(BGR)
     const size_t src_bytes = static_cast<size_t>(actual_src_stride) * static_cast<size_t>(bgr_frame.rows);
     const size_t dst_bytes = static_cast<size_t>(actual_dst_stride) * static_cast<size_t>(480);

     // 为RGA可能的内部对齐需求预留额外空间
     const size_t src_padding = 4096; // 预留4KB对齐空间
     const size_t dst_padding = 4096; // 预留4KB对齐空间
     const size_t aligned_src_bytes = src_bytes + src_padding;
     const size_t aligned_dst_bytes = dst_bytes + dst_padding;

     bool src_ok = runtime_cache.src_buffer.ensure(aligned_src_bytes);
     bool dst_ok = runtime_cache.dst_buffer.ensure(aligned_dst_bytes);
    if (src_ok && dst_ok) {
        unsigned char* src_ptr = reinterpret_cast<unsigned char*>(runtime_cache.src_buffer.addr());
        std::memset(src_ptr, 0, src_bytes);
        for (int y = 0; y < bgr_frame.rows; ++y) {
            std::memcpy(src_ptr + static_cast<size_t>(y) * static_cast<size_t>(src_wstride) * 3,
                        bgr_frame.ptr(y),
                        static_cast<size_t>(bgr_frame.cols) * 3);
        }

        rga_buffer_t src_img = wrapbuffer_handle(runtime_cache.src_buffer.handle(),
                                                 bgr_frame.cols,
                                                 bgr_frame.rows,
                                                 RK_FORMAT_BGR_888,
                                                 src_wstride,
                                                 bgr_frame.rows);
        rga_buffer_t dst_img = wrapbuffer_handle(runtime_cache.dst_buffer.handle(),
                                                 640,
                                                 480,
                                                 RK_FORMAT_BGR_888,
                                                 dst_wstride,
                                                 480);

        im_rect src_rect = {0, 0, bgr_frame.cols, bgr_frame.rows};
        im_rect dst_rect = {0, 0, 640, 480};
        IM_STATUS check_ret = imcheck(src_img, dst_img, src_rect, dst_rect);
        if (check_ret != IM_STATUS_NOERROR && check_ret != IM_STATUS_SUCCESS) {
            std::cerr << "[RGA] resize 参数预检失败: " << imStrError(check_ret)
                      << " src=" << bgr_frame.cols << "x" << bgr_frame.rows
                      << " src_wstride=" << src_wstride
                      << " dst_wstride=" << dst_wstride
                      << std::endl;
            context.rga_resize_fused_off = true;
            return false;
        }

        rga_status = imresize(src_img, dst_img);
        if (rga_status == IM_STATUS_SUCCESS) {
            cv::Mat resized_dma(480, 640, CV_8UC3, runtime_cache.dst_buffer.addr(), dst_wstride * 3);
            bgr_frame = resized_dma;
            return true;
        }
    } else {
        rga_status = IM_STATUS_OUT_OF_MEMORY;
    }

    context.rga_resize_fused_off = true;
    std::cerr << "[RGA] 缩放失败，已自动熔断为 CPU 缩放。错误: "
              << imStrError(rga_status) << " (" << static_cast<int>(rga_status) << ")"
              << std::endl;
    return false;
}
