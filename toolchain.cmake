set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_SYSROOT /home/fyj/rk3588/keneral/ubuntu/build/rootfs)

set(CMAKE_C_COMPILER /usr/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++)

# Set pkg-config to use sysroot
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_PATH} "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")

# Add linker flags to find libraries in sysroot
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-L${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu -Wl,-L${CMAKE_SYSROOT}/lib/aarch64-linux-gnu -Wl,--rpath-link=${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-L${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu -Wl,-L${CMAKE_SYSROOT}/lib/aarch64-linux-gnu -Wl,--rpath-link=${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu")

# OpenCV settings (manually configured since cmake config is missing)
set(OpenCV_FOUND TRUE)
set(OpenCV_VERSION "4.5.4")
set(OpenCV_INCLUDE_DIRS "${CMAKE_SYSROOT}/usr/include/opencv4")
set(OpenCV_LIBS opencv_core opencv_imgproc opencv_imgcodecs opencv_videoio opencv_video opencv_calib3d opencv_objdetect opencv_photo opencv_stitching opencv_dnn)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
