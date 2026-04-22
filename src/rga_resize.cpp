#include "rga_resize.h"

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

struct RgaRuntimeCache {
    Dma32RgaBuffer src_dma;
    Dma32RgaBuffer dst_dma;
};

RgaRuntimeCache& cacheForContext(RgaResizeContext* context) {
    static std::unordered_map<RgaResizeContext*, RgaRuntimeCache> caches;
    return caches[context];
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

    const int src_wstride = static_cast<int>((bgr_frame.step + 2) / 3);
    const size_t src_bytes = static_cast<size_t>(src_wstride) * static_cast<size_t>(bgr_frame.rows) * 3;
    const size_t dst_bytes = static_cast<size_t>(640) * static_cast<size_t>(480) * 3;

    bool src_ok = runtime_cache.src_dma.ensure(src_bytes, dma_heap_path);
    bool dst_ok = runtime_cache.dst_dma.ensure(dst_bytes, dma_heap_path);
    if (src_ok && dst_ok) {
        unsigned char* src_ptr = reinterpret_cast<unsigned char*>(runtime_cache.src_dma.addr());
        for (int y = 0; y < bgr_frame.rows; ++y) {
            std::memcpy(src_ptr + static_cast<size_t>(y) * static_cast<size_t>(src_wstride) * 3,
                        bgr_frame.ptr(y),
                        static_cast<size_t>(bgr_frame.cols) * 3);
        }

        rga_buffer_t src_img = wrapbuffer_handle(runtime_cache.src_dma.handle(),
                                                 bgr_frame.cols,
                                                 bgr_frame.rows,
                                                 RK_FORMAT_BGR_888,
                                                 src_wstride,
                                                 bgr_frame.rows);
        rga_buffer_t dst_img = wrapbuffer_handle(runtime_cache.dst_dma.handle(),
                                                 640,
                                                 480,
                                                 RK_FORMAT_BGR_888,
                                                 640,
                                                 480);
        rga_status = imresize(src_img, dst_img);
        if (rga_status == IM_STATUS_SUCCESS) {
            cv::Mat resized_dma(480, 640, CV_8UC3, runtime_cache.dst_dma.addr(), 640 * 3);
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
