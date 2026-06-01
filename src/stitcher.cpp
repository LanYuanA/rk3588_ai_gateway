#include "stitcher.h"

#include <array>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>

#include "app_context.h"

#include "im2d_version.h"
#include "im2d_buffer.h"
#include "im2d_common.h"
#include "im2d_single.h"
#include "im2d_type.h"
#include "rga.h"

//获取最新帧，输入参数是一个数组(帧队列，视频流数)
void updateCanvasFromPushQueues(std::array<VideoFrame, NUM_STREAMS>& latest_frames) {
    static int drop_log_counter = 0;
    VideoFrame frame;
    for (int i = 0; i < NUM_STREAMS; ++i) {
        // 清空队列中除了最新帧外的所有帧，确保获取到最新的视频帧
        size_t qsize = g_push_queues[i].size();
        if (qsize > 1) {
            int dropped = static_cast<int>(qsize) - 1;
            while (g_push_queues[i].size() > 1) {
                VideoFrame dummy;
                g_push_queues[i].pop(dummy);
            }
            drop_log_counter += dropped;
            if (drop_log_counter >= 30) {
                std::cerr << "[丢帧] push_queues[" << i << "] 累计丢弃 "
                          << drop_log_counter << " 帧（推流跟不上拉流）" << std::endl;
                drop_log_counter = 0;
            }
        }
        // 获取最新帧
        if (g_push_queues[i].size() > 0 && g_push_queues[i].pop(frame)) {
            latest_frames[i] = frame;
        }
    }
}

// ==================== 预分配 Grid + RGA buffer handle 导入 ====================

struct GridPool {
    cv::Mat grid;  // 预分配的 1280×960 BGR Grid（只分配一次，之后复用）
    bool initialized = false;

    void init() {
        if (initialized) return;
        grid = cv::Mat(960, 1280, CV_8UC3, cv::Scalar(0, 0, 0));
        initialized = true;
        std::cout << "[RGA-Grid] Grid 已预分配（复用模式）" << std::endl;
    }
};

static GridPool& gridPool() {
    static GridPool instance;
    return instance;
}

//画四个框图，并且非阻塞的获取人脸检测的最终结果，并显示在框图中
cv::Mat composeGridWithDetections(
    const std::array<VideoFrame, NUM_STREAMS> &latest_frames,
    int64_t unused_sync_reference) {

    GridPool& gp = gridPool();
    gp.init();  // 首次调用时初始化（之后直接跳过）

    // 清空 Grid（memset 3.6MB，比重新分配快）
    std::memset(gp.grid.data, 0, gp.grid.total() * gp.grid.elemSize());

    for (int i = 0; i < NUM_STREAMS; ++i) {
        if (latest_frames[i].image && !latest_frames[i].image->empty()) {
            const cv::Mat& cam = *latest_frames[i].image;

            // CPU copyTo（Grid 内存非连续，RGA imcopy 不支持普通堆内存）
            cv::Mat roi_mat = gp.grid(cv::Rect(i % 2 == 0 ? 0 : 640,
                                                i < 2 ? 0 : 480, 640, 480));
            cam.copyTo(roi_mat);

            // 获取检测结果（非阻塞）
            std::vector<DetectResult> current_res;
            if (g_results_mutex[i].try_lock()) {
                current_res = g_latest_results[i];
                g_results_mutex[i].unlock();
            }

            std::ostringstream info_text;
            info_text << "Stream: " << i;
            cv::putText(roi_mat,
                        info_text.str(),
                        cv::Point(10, 22),
                        cv::FONT_HERSHEY_SIMPLEX,
                        0.55,
                        cv::Scalar(255, 255, 0),
                        2);

            for (const auto& res : current_res) {
                cv::rectangle(roi_mat, res.box, cv::Scalar(0, 255, 0), 2);
                std::string label = "Face: " + std::to_string(res.confidence).substr(0, 4);
                cv::putText(roi_mat,
                            label,
                            cv::Point(res.box.x, res.box.y - 5),
                            cv::FONT_HERSHEY_SIMPLEX,
                            0.5,
                            cv::Scalar(0, 255, 0),
                            1);
            }
        }
    }

    // 在最终网格的右下角显示当前实时时间（精确到秒）
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_time), "%H:%M:%S");

    cv::putText(gp.grid,
                "Time: " + ss.str(),
                cv::Point(gp.grid.cols - 200, gp.grid.rows - 20),
                cv::FONT_HERSHEY_SIMPLEX,
                0.6,
                cv::Scalar(0, 255, 255),
                2);

    return gp.grid;
}

// ==================== RGA BGR → NV12 硬件转换 ====================

namespace {

struct GridRgaCache {
    bool initialized = false;
    bool fused_off = false;
    uint64_t scheduler_core = IM_SCHEDULER_RGA3_CORE1;

    // 源缓冲区（BGR，DMA内存）
    void* src_addr = nullptr;
    size_t src_size = 0;
    rga_buffer_handle_t src_handle = 0;

    // 目标缓冲区（NV12，DMA内存）
    void* dst_addr = nullptr;
    size_t dst_size = 0;
    rga_buffer_handle_t dst_handle = 0;

    ~GridRgaCache() { release(); }

    bool ensure(int width, int height) {
        const size_t bgr_bytes = static_cast<size_t>(width) * height * 3;
        const size_t nv12_bytes = static_cast<size_t>(width) * height * 3 / 2;

        if (initialized && src_size >= bgr_bytes && dst_size >= nv12_bytes) return true;
        release();

        // 分配源缓冲区（BGR DMA内存）
        src_addr = mmap(nullptr, bgr_bytes + 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (src_addr == MAP_FAILED) { src_addr = nullptr; return false; }
        src_size = bgr_bytes + 4096;

        src_handle = importbuffer_virtualaddr(src_addr, static_cast<int>(src_size));
        if (src_handle == 0) { release(); return false; }

        // 分配目标缓冲区（NV12 DMA内存）
        dst_addr = mmap(nullptr, nv12_bytes + 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (dst_addr == MAP_FAILED) { dst_addr = nullptr; release(); return false; }
        dst_size = nv12_bytes + 4096;

        dst_handle = importbuffer_virtualaddr(dst_addr, static_cast<int>(dst_size));
        if (dst_handle == 0) { release(); return false; }

        initialized = true;
        return true;
    }

    void release() {
        if (src_handle != 0) { releasebuffer_handle(src_handle); src_handle = 0; }
        if (src_addr && src_addr != MAP_FAILED) { munmap(src_addr, src_size); src_addr = nullptr; }
        src_size = 0;

        if (dst_handle != 0) { releasebuffer_handle(dst_handle); dst_handle = 0; }
        if (dst_addr && dst_addr != MAP_FAILED) { munmap(dst_addr, dst_size); dst_addr = nullptr; }
        dst_size = 0;

        initialized = false;
    }
};

GridRgaCache& gridRgaCache() {
    static GridRgaCache cache;
    return cache;
}

} // namespace

bool convertGridBgrToNv12WithRga(const cv::Mat& bgr_grid, cv::Mat& nv12_out) {
    GridRgaCache& cache = gridRgaCache();
    if (cache.fused_off) return false;

    const int w = bgr_grid.cols;
    const int h = bgr_grid.rows;

    if (!cache.ensure(w, h)) {
        cache.fused_off = true;
        return false;
    }

    // 将cv::Mat数据拷贝到DMA源缓冲区
    const size_t bgr_bytes = static_cast<size_t>(w) * h * 3;
    std::memcpy(cache.src_addr, bgr_grid.data, bgr_bytes);

    // 源和目标都使用DMA handle
    rga_buffer_t src_img = wrapbuffer_handle(cache.src_handle, w, h,
                                              RK_FORMAT_BGR_888, w, h);
    rga_buffer_t dst_img = wrapbuffer_handle(cache.dst_handle, w, h,
                                              RK_FORMAT_YCbCr_420_SP, w, h);

    im_rect rect = {0, 0, w, h};
    IM_STATUS check_ret = imcheck(src_img, dst_img, rect, rect);
    if (check_ret != IM_STATUS_NOERROR && check_ret != IM_STATUS_SUCCESS) {
        std::cerr << "[RGA-Grid] imcheck失败: " << imStrError(check_ret) << std::endl;
        cache.fused_off = true;
        return false;
    }

    IM_STATUS ret = imcvtcolor(src_img, dst_img, RK_FORMAT_BGR_888, RK_FORMAT_YCbCr_420_SP);
    if (ret == IM_STATUS_SUCCESS) {
        const size_t nv12_bytes = static_cast<size_t>(w) * h * 3 / 2;
        nv12_out.create(h * 3 / 2, w, CV_8UC1);
        std::memcpy(nv12_out.data, cache.dst_addr, nv12_bytes);
        return true;
    }

    cache.fused_off = true;
    std::cerr << "[RGA-Grid] BGR→NV12 转换失败，熔断到 CPU。错误: "
              << imStrError(ret) << std::endl;
    return false;
}

bool convertGridBgrToNv12WithRga(const cv::Mat& bgr_grid, void* dst_buf, size_t dst_capacity) {
    GridRgaCache& cache = gridRgaCache();
    if (cache.fused_off) return false;

    const int w = bgr_grid.cols;
    const int h = bgr_grid.rows;
    const size_t nv12_bytes = static_cast<size_t>(w) * h * 3 / 2;
    if (dst_capacity < nv12_bytes) return false;

    const size_t bgr_bytes = static_cast<size_t>(w) * h * 3;
    if (!cache.ensure(w, h)) {
        cache.fused_off = true;
        return false;
    }

    // 将cv::Mat数据拷贝到DMA源缓冲区
    std::memcpy(cache.src_addr, bgr_grid.data, bgr_bytes);

    // 源使用DMA handle，目标使用用户传入的虚拟地址
    rga_buffer_t src_img = wrapbuffer_handle(cache.src_handle, w, h,
                                              RK_FORMAT_BGR_888, w, h);
    rga_buffer_t dst_img = wrapbuffer_virtualaddr(dst_buf, w, h, RK_FORMAT_YCbCr_420_SP);

    im_rect rect = {0, 0, w, h};
    IM_STATUS check_ret = imcheck(src_img, dst_img, rect, rect);
    if (check_ret != IM_STATUS_NOERROR && check_ret != IM_STATUS_SUCCESS) {
        cache.fused_off = true;
        return false;
    }

    IM_STATUS ret = imcvtcolor(src_img, dst_img, RK_FORMAT_BGR_888, RK_FORMAT_YCbCr_420_SP);
    if (ret == IM_STATUS_SUCCESS) {
        return true;
    }

    cache.fused_off = true;
    std::cerr << "[RGA-Grid] BGR→NV12 转换失败，熔断到 CPU。错误: "
              << imStrError(ret) << std::endl;
    return false;
}
