#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <chrono>
#include <mutex>

class TimeManager {
public:
    static TimeManager& getInstance() {
        static TimeManager instance;
        return instance;
    }

    // 获取格式化的当前时间字符串（精确到秒）
    std::string getCurrentTimeString();

    // 重置参考时间（用于校准）
    void resetReferenceTime();

private:
    TimeManager();  // 私有构造函数
    
    std::chrono::system_clock::time_point reference_time_;
    std::chrono::steady_clock::time_point reference_steady_time_;
    std::mutex mutex_;
    
    // 校准间隔（1分钟）
    static constexpr long CALIBRATION_INTERVAL_MS = 60000;
};

#endif // TIME_MANAGER_H