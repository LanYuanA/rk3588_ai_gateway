cmake_minimum_required(VERSION 3.5)
project(rga_allocator_dma32_demo)

set(CMAKE_CXX_STANDARD 11)

find_package(OpenCV REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(rga_allocator_dma32_demo
    rga_allocator_dma32_demo.cpp
    dma_alloc.cpp
    utils.cpp
)

target_link_libraries(rga_allocator_dma32_demo /usr/lib/aarch64-linux-gnu/librga.so.2)
target_link_libraries(rga_allocator_dma32_demo ${OpenCV_LIBS})
