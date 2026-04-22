#pragma once

#include <string>
#include <vector>

void inferenceThread(const std::string& model_path,
                     std::vector<int> handled_streams,
                     int npu_thread_id,
                     int npu_core_index);
