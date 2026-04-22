#include "app_runtime.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

bool envFlagEnabled(const char* key, bool default_value = false) {
    const char* value = std::getenv(key);
    if (!value || value[0] == '\0') {
        return default_value;
    }
    return std::strcmp(value, "1") == 0 ||
           std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "TRUE") == 0 ||
           std::strcmp(value, "yes") == 0 ||
           std::strcmp(value, "YES") == 0;
}

int envIntValue(const char* key, int default_value) {
    const char* value = std::getenv(key);
    if (!value || value[0] == '\0') {
        return default_value;
    }
    char* endptr = nullptr;
    long parsed = std::strtol(value, &endptr, 10);
    if (endptr == value) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

std::string envStringValue(const char* key, const std::string& default_value) {
    const char* value = std::getenv(key);
    if (!value || value[0] == '\0') {
        return default_value;
    }
    return std::string(value);
}

} // namespace

RuntimeOptions loadRuntimeOptions() {
    RuntimeOptions options;
    options.pin_threads = envFlagEnabled("RK_PIN_THREADS", false);
    options.use_rga_resize = envFlagEnabled("RK_USE_RGA_RESIZE", true);
    options.use_dma32_for_rga = envFlagEnabled("RK_RGA_USE_DMA32", true);
    options.dma_heap_path = envStringValue("RK_DMA_HEAP_PATH", "/dev/dma_heap/system-dma32");
    options.rga_scheduler_mode = envIntValue("RK_RGA_SCHEDULER", -1);
    options.use_rga2_mode = (options.rga_scheduler_mode == 3);
    return options;
}

std::vector<std::string> buildDefaultStreamSources() {
    return {
        "/dev/video74",
        "rtsp://127.0.0.1:8554/host_cam",
        "rtsp://127.0.0.1:8554/host_cam2",
        "rtsp://127.0.0.1:8554/host_cam3"
    };
}

bool validateStreamSources(const std::vector<std::string>& stream_sources, int expected_streams) {
    if (static_cast<int>(stream_sources.size()) != expected_streams) {
        std::cerr << "[错误] 配置的流数量(" << stream_sources.size()
                  << ") 与 NUM_STREAMS(" << expected_streams << ") 不一致。" << std::endl;
        return false;
    }
    return true;
}

void printRuntimeOptions(const RuntimeOptions& options) {
    if (options.pin_threads) {
        std::cout << "[线程调度] 已启用绑核模式 (RK_PIN_THREADS=1)" << std::endl;
    } else {
        std::cout << "[线程调度] 使用系统默认调度，避免用户线程与硬件 IRQ 长时间抢占" << std::endl;
    }

    if (options.use_rga_resize) {
        std::cout << "[图像缩放] 已启用 RGA 缩放 (RK_USE_RGA_RESIZE=1)" << std::endl;
        std::cout << "[RGA调度] RK_RGA_SCHEDULER=" << options.rga_scheduler_mode
                  << " (0=RGA3_CORE0,1=RGA3_CORE1,2=RGA3双核,3=RGA2_CORE0,-1=按流自动分配)" << std::endl;
        if (options.use_rga2_mode) {
            std::cout << "[RGA内存] RGA2 模式下强制使用 demo DMA32 路径, RK_DMA_HEAP_PATH="
                      << options.dma_heap_path << std::endl;
        } else {
            std::cout << "[RGA内存] 已强制 DMA32 模式 (RK_RGA_USE_DMA32=1), RK_DMA_HEAP_PATH="
                      << options.dma_heap_path << std::endl;
            if (!options.use_dma32_for_rga) {
                std::cout << "[RGA内存] 警告: 检测到 RK_RGA_USE_DMA32=0，但已被忽略以防止 >4G 风险" << std::endl;
            }
        }
    } else {
        std::cout << "[图像缩放] 已禁用 RGA，使用 CPU 缩放" << std::endl;
    }
}
