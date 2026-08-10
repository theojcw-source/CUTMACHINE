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

add_library(cutmachine_model
    src/Cli.cc
    src/Document.cc
    src/EditLog.cc
    src/Operations.cc
    src/Timeline.cc
    src/Ulid.cc
)
target_include_directories(cutmachine_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(cutmachine
    src/main.mm
    src/DecodeWorker.cc
    src/FrameCache.cc
    src/PerformanceMetrics.cc
    src/MediaSource.mm
    src/Renderer.mm
)
target_include_directories(cutmachine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cutmachine PRIVATE cutmachine_model)
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

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    add_executable(cutmachine_tests tests/model_tests.cc)
    target_link_libraries(cutmachine_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_model_tests COMMAND cutmachine_tests)

    add_executable(cutmachine_edit_tests tests/edit_tests.cc)
    target_link_libraries(cutmachine_edit_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_edit_tests COMMAND cutmachine_edit_tests)

    add_executable(cutmachine_cli_tests tests/cli_tests.cc)
    target_link_libraries(cutmachine_cli_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_cli_tests COMMAND cutmachine_cli_tests)

    add_test(
        NAME cutmachine_sidecar_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest sidecar.test_sidecar
    )
    set_tests_properties(cutmachine_sidecar_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
