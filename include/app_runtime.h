#pragma once

#include <string>
#include <vector>

struct RuntimeOptions {
    bool pin_threads = false;
    bool use_rga_resize = true;
    bool use_dma32_for_rga = true;
    std::string dma_heap_path = "/dev/dma_heap/system-dma32";
    int rga_scheduler_mode = -1;
    bool use_rga2_mode = false;
};

RuntimeOptions loadRuntimeOptions();
std::vector<std::string> buildDefaultStreamSources();
bool validateStreamSources(const std::vector<std::string>& stream_sources, int expected_streams);
void printRuntimeOptions(const RuntimeOptions& options);
