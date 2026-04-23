#include "time_manager.h"

#include <chrono>
#include <iomanip>
#include <sstream>

TimeManager::TimeManager() {
    resetReferenceTime();
}

std::string TimeManager::getCurrentTimeString() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 计算从参考时间点经过的持续时间
    auto current_steady = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        current_steady - reference_steady_time_).count();
    
    // 检查是否需要校准（每分钟校准一次）
    if (elapsed_ms > CALIBRATION_INTERVAL_MS) {
        resetReferenceTime();
        // 重新获取经过的时间
        current_steady = std::chrono::steady_clock::now();
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_steady - reference_steady_time_).count();
    }
    
    // 基于参考时间和经过的持续时间计算当前系统时间
    auto current_time = reference_time_ + std::chrono::milliseconds(elapsed_ms);
    
    // 格式化时间为字符串（精确到秒）
    std::time_t time_t_current = std::chrono::system_clock::to_time_t(current_time);
    std::tm tm_current = *std::localtime(&time_t_current);
    
    std::ostringstream ss;
    ss << std::put_time(&tm_current, "%H:%M:%S");
    
    return ss.str();
}

void TimeManager::resetReferenceTime() {
    reference_time_ = std::chrono::system_clock::now();
    reference_steady_time_ = std::chrono::steady_clock::now();
}