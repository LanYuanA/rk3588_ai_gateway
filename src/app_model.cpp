#include "app_model.h"

#include <fstream>
#include <limits.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <cstdlib>

namespace {

bool fileExists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

std::string getExecutableDir() {
    char exe_path[PATH_MAX] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) {
        return ".";
    }
    exe_path[len] = '\0';
    std::string full_path(exe_path);
    size_t last_slash = full_path.find_last_of('/');
    if (last_slash == std::string::npos) {
        return ".";
    }
    return full_path.substr(0, last_slash);
}

} // namespace

std::string resolveModelPath() {
    const char* env_model = std::getenv("RKNN_MODEL_PATH");
    if (env_model && env_model[0] != '\0' && fileExists(env_model)) {
        return std::string(env_model);
    }

    const std::string exe_dir = getExecutableDir();
    const std::vector<std::string> candidates = {
        "./yolov8_face_fp.rknn",
        "../yolov8_face_fp.rknn",
        exe_dir + "/yolov8_face_fp.rknn",
        exe_dir + "/../yolov8_face_fp.rknn"
    };

    for (const auto& path : candidates) {
        if (fileExists(path)) {
            return path;
        }
    }

    return "";
}
