cmake_minimum_required(VERSION 3.24)
project(CUTMACHINE LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
)

set(SHADER_RUNTIME_PATH "${CMAKE_CURRENT_BINARY_DIR}/shader.metal")
configure_file(src/shader.metal "${SHADER_RUNTIME_PATH}" COPYONLY)

add_executable(cutmachine
    src/main.mm
    src/DecodeWorker.cc
    src/FrameCache.cc
    src/PerformanceMetrics.cc
    src/MediaSource.mm
    src/Renderer.mm
)
target_include_directories(cutmachine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(cutmachine PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)
target_compile_definitions(cutmachine PRIVATE
    CUTMACHINE_SHADER_PATH="${SHADER_RUNTIME_PATH}"
)
target_link_libraries(cutmachine PRIVATE
    PkgConfig::FFMPEG
    "-framework AppKit"
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore"
)
