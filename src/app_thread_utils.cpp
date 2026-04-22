#include "app_thread_utils.h"

#include <iostream>
#include <pthread.h>

void bindThreadToCpu(std::thread& t, int cpu_id, const std::string& tag) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    int ret = pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
    if (ret != 0) {
        std::cerr << "[线程绑核警告] " << tag << " 绑定 CPU " << cpu_id
                  << " 失败，错误码: " << ret << std::endl;
    }
}
