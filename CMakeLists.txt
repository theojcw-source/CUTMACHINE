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
    libswresample
)

set(SHADER_RUNTIME_PATH "${CMAKE_CURRENT_BINARY_DIR}/shader.metal")
configure_file(src/shader.metal "${SHADER_RUNTIME_PATH}" COPYONLY)

add_library(cutmachine_model
    src/Cli.cc
    src/ColorManagement.cc
    src/Document.cc
    src/Ingest.cc
    src/EditLog.cc
    src/Export.cc
    src/Keyframes.cc
    src/Operations.cc
    src/ProjectRecovery.cc
    src/Timeline.cc
    src/TimelineView.cc
    src/Ulid.cc
)
target_include_directories(cutmachine_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cutmachine_model PUBLIC PkgConfig::FFMPEG)

add_executable(cutmachine
    src/main.mm
    src/AudioPlayback.mm
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
    "-framework AVFAudio"
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework UniformTypeIdentifiers"
)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    get_target_property(_ffmpeg_includes PkgConfig::FFMPEG
                        INTERFACE_INCLUDE_DIRECTORIES)
    list(GET _ffmpeg_includes 0 _ffmpeg_include)
    get_filename_component(_ffmpeg_prefix "${_ffmpeg_include}" DIRECTORY)
    find_program(FFMPEG_EXECUTABLE NAMES ffmpeg
                 HINTS "${_ffmpeg_prefix}/bin" REQUIRED)

    add_executable(cutmachine_tests tests/model_tests.cc)
    target_link_libraries(cutmachine_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_model_tests COMMAND cutmachine_tests)

    add_executable(cutmachine_edit_tests tests/edit_tests.cc)
    target_link_libraries(cutmachine_edit_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_edit_tests COMMAND cutmachine_edit_tests)

    add_executable(cutmachine_timeline_view_tests
                   tests/timeline_view_tests.cc)
    target_link_libraries(cutmachine_timeline_view_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_timeline_view_tests
             COMMAND cutmachine_timeline_view_tests)

    add_executable(cutmachine_cli_tests tests/cli_tests.cc)
    target_link_libraries(cutmachine_cli_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_cli_tests COMMAND cutmachine_cli_tests)

    add_executable(cutmachine_keyframe_tests tests/keyframe_tests.cc)
    target_link_libraries(cutmachine_keyframe_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_keyframe_tests COMMAND cutmachine_keyframe_tests)

    add_executable(cutmachine_project_recovery_tests
                   tests/project_recovery_tests.cc)
    target_link_libraries(cutmachine_project_recovery_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_recovery_tests
             COMMAND cutmachine_project_recovery_tests)

    add_executable(cutmachine_export_tests tests/export_tests.cc)
    target_link_libraries(cutmachine_export_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_export_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
    )
    add_test(NAME cutmachine_export_tests COMMAND cutmachine_export_tests)

    add_executable(cutmachine_ingest_tests tests/ingest_tests.cc)
    target_link_libraries(cutmachine_ingest_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_ingest_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_ingest_tests COMMAND cutmachine_ingest_tests)

    add_executable(cutmachine_audio_playback_tests
        tests/audio_playback_tests.mm
        src/AudioPlayback.mm
    )
    target_compile_options(cutmachine_audio_playback_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(cutmachine_audio_playback_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
    )
    target_link_libraries(cutmachine_audio_playback_tests PRIVATE
        cutmachine_model
        PkgConfig::FFMPEG
        "-framework AVFAudio"
        "-framework Foundation"
    )
    add_test(NAME cutmachine_audio_playback_tests
             COMMAND cutmachine_audio_playback_tests)

    add_test(
        NAME cutmachine_sidecar_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest sidecar.test_sidecar
    )
    set_tests_properties(cutmachine_sidecar_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
