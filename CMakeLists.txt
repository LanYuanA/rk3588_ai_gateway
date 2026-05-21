cmake_minimum_required(VERSION 3.10)

# 自动检测交叉编译环境 (Docker 容器中 SYSROOT=/mnt)
# 必须在 project() 之前设置编译器，否则 CMake 用宿主机编译器检测 ABI
if(DEFINED ENV{SYSROOT} AND EXISTS "$ENV{SYSROOT}/usr/include")
    set(CMAKE_C_COMPILER "aarch64-linux-gnu-gcc")
    set(CMAKE_CXX_COMPILER "aarch64-linux-gnu-g++")
    set(CMAKE_SYSROOT $ENV{SYSROOT})
    set(CMAKE_FIND_ROOT_PATH $ENV{SYSROOT})
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    message(STATUS "检测到交叉编译环境, SYSROOT=${CMAKE_SYSROOT}")
endif()

project(RK3588_AI_Gateway)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SYSROOT)
    set(OpenCV_DIR "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/cmake/opencv4" CACHE PATH "")
    set(_LINK_DIRS
        "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu"
        "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/blas"
        "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/lapack"
        "${CMAKE_SYSROOT}/lib/aarch64-linux-gnu"
        "${CMAKE_SYSROOT}/lib"
    )
    set(_LINK_FLAGS "")
    foreach(_dir ${_LINK_DIRS})
        string(APPEND _LINK_FLAGS " -L${_dir} -Wl,--rpath-link=${_dir}")
    endforeach()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_LINK_FLAGS}" CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
find_library(RKNN_RUNTIME rknnrt)

include_directories(${CMAKE_SOURCE_DIR}/include ${OpenCV_INCLUDE_DIRS})
include_directories(${GSTREAMER_INCLUDE_DIRS})

set(SOURCES
    src/main.cpp
    src/app_model.cpp
    src/app_runtime.cpp
    src/app_context.cpp
    src/app_lifecycle.cpp
    src/app_thread_utils.cpp
    src/puller_thread.cpp
    src/streamer_thread.cpp
    src/inference_thread.cpp
    src/rtsp_mpp_decoder.cpp
    src/rknn_detector.cpp
    src/stitcher.cpp
    src/rga_resize.cpp
    src/rga_yuv_converter.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

find_library(RGA_LIB rga PATHS /usr/lib/aarch64-linux-gnu /mnt/usr/lib/aarch64-linux-gnu)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads ${OpenCV_LIBS} ${RKNN_RUNTIME} ${RGA_LIB} ${GSTREAMER_LIBRARIES})
