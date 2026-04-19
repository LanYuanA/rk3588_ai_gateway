cmake_minimum_required(VERSION 3.10)
project(RK3588_AI_Gateway)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 寻找多线程库
find_package(Threads REQUIRED)

# 寻找 OpenCV 库 (用于真实的视频流拉取和图像操作)
find_package(OpenCV REQUIRED)

# 寻找 RKNN Runtime 库
find_library(RKNN_RUNTIME rknnrt)

# 设置头文件包含路径 (请确保环境里有 rknn_api.h，通常在 /usr/include/)
include_directories(${CMAKE_SOURCE_DIR}/include ${OpenCV_INCLUDE_DIRS})

# 收集所有的源文件
file(GLOB_RECURSE SOURCES "src/*.cpp")

# 生成可执行文件
add_executable(${PROJECT_NAME} ${SOURCES})

# 链接依赖库
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads ${OpenCV_LIBS} ${RKNN_RUNTIME} /usr/lib/aarch64-linux-gnu/librga.so.2)
