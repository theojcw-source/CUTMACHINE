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
# The MCP server (HttpServer.cc/McpServer.cc) uses std::thread for its
# accept loop.
find_package(Threads REQUIRED)

set(SHADER_RUNTIME_PATH "${CMAKE_CURRENT_BINARY_DIR}/shader.metal")
configure_file(src/shader.metal "${SHADER_RUNTIME_PATH}" COPYONLY)

add_library(cutmachine_model
    src/Cli.cc
    src/ColorManagement.cc
    src/Document.cc
    src/HttpServer.cc
    src/IdResolver.cc
    src/Ingest.cc
    src/Json.cc
    src/MediaTaskManager.cc
    src/EditLog.cc
    src/Export.cc
    src/Keyframes.cc
    src/McpProjectBackend.cc
    src/McpServer.cc
    src/McpTools.cc
    src/Operations.cc
    src/Project.cc
    src/ProjectBin.cc
    src/ProjectRecovery.cc
    src/ProjectStorage.cc
    src/Proxy.cc
    src/Relink.cc
    src/Thumbnail.cc
    src/Timeline.cc
    src/TimelineView.cc
    src/Ulid.cc
    src/Waveform.cc
)
target_include_directories(cutmachine_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cutmachine_model PUBLIC PkgConfig::FFMPEG Threads::Threads)

set(CUTMACHINE_APP_SOURCES
    src/main.mm
    src/AudioPlayback.mm
    src/DecodeWorker.cc
    src/FrameCache.cc
    src/PerformanceMetrics.cc
    src/MediaSource.mm
    src/Renderer.mm
)

add_executable(cutmachine ${CUTMACHINE_APP_SOURCES})
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

# A regular macOS application for Finder/Dock launches. The plain `cutmachine`
# executable remains available for the CLI commands and automated workflows.
add_executable(cutmachine_app MACOSX_BUNDLE
    ${CUTMACHINE_APP_SOURCES}
    src/shader.metal
)
set_source_files_properties(src/shader.metal PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)
set_target_properties(cutmachine_app PROPERTIES
    OUTPUT_NAME CUTMACHINE
    MACOSX_BUNDLE_BUNDLE_NAME CUTMACHINE
    MACOSX_BUNDLE_GUI_IDENTIFIER com.cutmachine.editor
    MACOSX_BUNDLE_BUNDLE_VERSION 1
    MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
)
target_include_directories(cutmachine_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cutmachine_app PRIVATE cutmachine_model)
target_compile_options(cutmachine_app PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)
target_compile_definitions(cutmachine_app PRIVATE
    CUTMACHINE_SHADER_PATH="${SHADER_RUNTIME_PATH}"
)
target_link_libraries(cutmachine_app PRIVATE
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

    # Pure-C++ MCP tool-dispatch/HTTP/JSON-RPC test (ROADMAP.md F1.1/F1.2):
    # no ProjectStorage, so it also builds and runs on a plain Linux host --
    # see tests/mcp_tools_tests.cc for details.
    add_executable(cutmachine_mcp_tools_tests tests/mcp_tools_tests.cc)
    target_link_libraries(cutmachine_mcp_tools_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_mcp_tools_tests COMMAND cutmachine_mcp_tools_tests)

    # Integration test against the real project-file backend
    # (McpProjectBackend/ProjectStorage) -- macOS only, like cutmachine_cli_tests.
    add_executable(cutmachine_mcp_tests tests/mcp_tests.cc)
    target_link_libraries(cutmachine_mcp_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_mcp_tests COMMAND cutmachine_mcp_tests)

    add_test(
        NAME cutmachine_architecture_tests
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture_tests.py
                ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_executable(cutmachine_keyframe_tests tests/keyframe_tests.cc)
    target_link_libraries(cutmachine_keyframe_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_keyframe_tests COMMAND cutmachine_keyframe_tests)

    add_executable(cutmachine_project_recovery_tests
                   tests/project_recovery_tests.cc)
    target_link_libraries(cutmachine_project_recovery_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_recovery_tests
             COMMAND cutmachine_project_recovery_tests)

    add_executable(cutmachine_project_tests tests/project_tests.cc)
    target_link_libraries(cutmachine_project_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_tests COMMAND cutmachine_project_tests)

    add_executable(cutmachine_project_storage_tests
                   tests/project_storage_tests.cc)
    target_link_libraries(cutmachine_project_storage_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_storage_tests
             COMMAND cutmachine_project_storage_tests)

    add_executable(cutmachine_project_bin_tests tests/project_bin_tests.cc)
    target_link_libraries(cutmachine_project_bin_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_bin_tests COMMAND cutmachine_project_bin_tests)

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

    add_executable(cutmachine_media_task_manager_tests
        tests/media_task_manager_tests.cc
    )
    target_link_libraries(cutmachine_media_task_manager_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_media_task_manager_tests
             COMMAND cutmachine_media_task_manager_tests)

    add_executable(cutmachine_proxy_tests tests/proxy_tests.cc)
    target_link_libraries(cutmachine_proxy_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_proxy_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_proxy_tests COMMAND cutmachine_proxy_tests)

    add_executable(cutmachine_waveform_tests tests/waveform_tests.cc)
    target_link_libraries(cutmachine_waveform_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_waveform_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_waveform_tests COMMAND cutmachine_waveform_tests)

    add_executable(cutmachine_thumbnail_tests tests/thumbnail_tests.cc)
    target_link_libraries(cutmachine_thumbnail_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_thumbnail_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_thumbnail_tests COMMAND cutmachine_thumbnail_tests)

    add_executable(cutmachine_relink_tests tests/relink_tests.cc)
    target_link_libraries(cutmachine_relink_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_relink_tests COMMAND cutmachine_relink_tests)

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
