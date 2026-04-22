#include "app_context.h"
#include "app_lifecycle.h"
#include "app_model.h"
#include "app_runtime.h"

#include <csignal>
#include <iostream>
#include <string>
#include <vector>

int main() {
    signal(SIGPIPE, SIG_IGN);
    std::cout << "========== RK3588 AI Gateway 初始化 ==========\n";

    const std::string model_path = resolveModelPath();
    if (model_path.empty()) {
        std::cerr << "[错误] 未找到 RKNN 模型文件 yolov8_face_fp.rknn。" << std::endl;
        std::cerr << "[提示] 请将模型放在可执行文件目录或其上级目录，或设置环境变量 RKNN_MODEL_PATH。" << std::endl;
        return 1;
    }
    std::cout << "[RKNN] 使用模型文件: " << model_path << std::endl;

    const std::vector<std::string> stream_sources = buildDefaultStreamSources();
    if (!validateStreamSources(stream_sources, NUM_STREAMS)) {
        return 1;
    }

    const RuntimeOptions options = loadRuntimeOptions();
    printRuntimeOptions(options);

    WorkerThreads workers;
    startWorkerThreads(stream_sources, model_path, options, workers);

    std::cout << "所有子线程已启动，按 Enter 键安全退出系统...\n";
    std::cin.get();

    requestShutdown();
    joinWorkerThreads(workers);

    std::cout << "系统安全关闭. Bye!\n";
    return 0;
}
