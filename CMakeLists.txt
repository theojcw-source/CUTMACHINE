cmake_minimum_required(VERSION 3.24)
project(CUTMACHINE LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ language standard" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswresample
    libswscale
)
# The MCP server (HttpServer.cc/McpServer.cc) uses std::thread for its
# accept loop.
find_package(Threads REQUIRED)

# F1.4 -- local transcription. Fetched, not vendored, following this
# project's general preference for build-time dependencies over committed
# third-party source. whisper.cpp is MIT licensed (compatible with this
# project's AGPL-3.0-or-later) and runs entirely on-device: pulling it in does
# not touch PHILOSOPHY.md's "pas de service central" guard-rail, since inference
# still happens against a local model file the caller supplies, never a
# network call. Examples/tests/server and the GPU backends this project's
# target platform (macOS/Apple Silicon) does not use are disabled; GGML_METAL
# is left at its own default, which auto-enables on Apple platforms and
# stays off elsewhere.
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_CUDA OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    whispercpp
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG        v1.7.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(whispercpp)

# COLOR-2026-08 -- OpenColorIO owns the project color transforms rather than
# keeping a second, hand-written approximation in the player and exporter.
# Pin both the library and the built-in config selected in ColorManagement.cc:
# upgrading either changes rendered pixels and therefore needs an explicit
# project decision. Only the C++ core is built; CUTMACHINE neither ships nor
# needs OCIO's applications, language bindings, plug-ins, or own test suite.
set(OCIO_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_GPU_TESTS OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_JAVA OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_NUKE OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_OPENFX OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OCIO_INSTALL_EXT_PACKAGES ALL CACHE STRING "" FORCE)
FetchContent_Declare(
    opencolorio
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenColorIO.git
    GIT_TAG        v2.5.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(opencolorio)
set(OCIO_LICENSE_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/OpenColorIO-LICENSE")
configure_file("${opencolorio_SOURCE_DIR}/LICENSE" "${OCIO_LICENSE_RESOURCE}"
    COPYONLY)

# Treat whisper.cpp's headers as system headers. Without this they are
# ordinary includes, so every translation unit that pulls in whisper.h
# inherits ggml's warnings -- and .clang-tidy's HeaderFilterRegex
# ('.*/src/.*') matches build/_deps/whispercpp-src/ggml/src/... just as
# happily as this project's own src/, burying real findings under hundreds
# of lines about a dependency we do not maintain.
get_target_property(WHISPER_INTERFACE_INCLUDES whisper
                    INTERFACE_INCLUDE_DIRECTORIES)
if(WHISPER_INTERFACE_INCLUDES)
    set_target_properties(whisper PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${WHISPER_INTERFACE_INCLUDES}")
endif()

set(SHADER_RUNTIME_PATH "${CMAKE_CURRENT_BINARY_DIR}/shader.metal")
configure_file(src/shader.metal "${SHADER_RUNTIME_PATH}" COPYONLY)

# UI-2026-08 -- Lucide stays a build-time input. `sips` is part of macOS and
# rasterizes each SVG to a high-resolution RGBA mask; Renderer.mm packs these
# PNG alphas beside the CoreText glyphs in one R8 atlas. No SVG parser ships in
# the application and the generated files remain build artifacts.
find_program(SIPS_EXECUTABLE NAMES sips REQUIRED)
set(CUTMACHINE_ICON_NAMES
    chevrons-right-left
    eye
    hand
    headphones
    link
    lock
    magnet
    mouse-pointer-2
    scissors
    search
    unfold-horizontal
    volume-x
)
set(CUTMACHINE_ICON_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/ui-icons")
set(CUTMACHINE_ICON_PNGS)
foreach(_icon IN LISTS CUTMACHINE_ICON_NAMES)
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/${_icon}.svg")
    set(_output "${CUTMACHINE_ICON_OUTPUT_DIR}/${_icon}.png")
    add_custom_command(
        OUTPUT "${_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CUTMACHINE_ICON_OUTPUT_DIR}"
        COMMAND "${SIPS_EXECUTABLE}" -s format png -z 48 48 "${_source}"
                --out "${_output}"
        DEPENDS "${_source}"
        COMMENT "Rasterizing Lucide icon ${_icon} @2x"
        VERBATIM
    )
    list(APPEND CUTMACHINE_ICON_PNGS "${_output}")
endforeach()
add_custom_target(cutmachine_ui_icons DEPENDS ${CUTMACHINE_ICON_PNGS})

add_library(cutmachine_model
    src/BeatDetection.cc
    src/ChatLlmClient.cc
    src/ChatSession.cc
    src/Cli.cc
    src/ColorEffects.cc
    src/ColorManagement.cc
    src/Document.cc
    src/HttpServer.cc
    src/IdResolver.cc
    src/Ingest.cc
    src/InterviewShort.cc
    src/Json.cc
    src/LocalEnv.cc
    src/MediaTaskManager.cc
    src/EditLog.cc
    src/Export.cc
    src/FrameCapture.cc
    src/Keyframes.cc
    src/McpLiveBackend.cc
    src/McpProjectBackend.cc
    src/McpServer.cc
    src/McpTools.cc
    src/Operations.cc
    src/PauseTightening.cc
    src/Project.cc
    src/ProjectBin.cc
    src/ProjectRecovery.cc
    src/ProjectStorage.cc
    src/Proxy.cc
    src/Relink.cc
    src/ResolveExport.cc
    src/ResolveImport.cc
    src/SequenceFormat.cc
    src/SourceAddress.cc
    src/DocumentDelta.cc
    src/ShotQuality.cc
    src/TranscriptAlignment.cc
    src/SpeechOnset.cc
    src/Subtitles.cc
    src/Thumbnail.cc
    src/Timeline.cc
    src/TimelineStats.cc
    src/TimelineView.cc
    src/Transcription.cc
    src/Ulid.cc
    src/VideoScopes.cc
    src/Waveform.cc
)
target_include_directories(cutmachine_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cutmachine_model PUBLIC
    PkgConfig::FFMPEG
    OpenColorIO
    whisper
    Threads::Threads
)

set(CUTMACHINE_APP_SOURCES
    src/main.mm
    src/AudioPlayback.mm
    src/DecodeWorker.cc
    src/FrameCache.cc
    src/PerformanceMetrics.cc
    src/MediaSource.mm
    src/Renderer.mm
    src/MetalUiAtlas.mm
    # F2.1 -- design system (ROADMAP.md). UiTheme.h/PanelLayout.h are plain
    # C++ (see tests/ui_theme_tests.cc, tests/panel_layout_tests.cc) and
    # need no .cc of their own; everything below is AppKit/Foundation-only.
    src/UiThemeAppKit.mm
    src/UiPreferences.mm
    src/UiComponents.mm
    src/PanelHostView.mm
    # F2.2 -- Inspector (ROADMAP.md). InspectorGradeControls.h is plain
    # C++ (see tests/inspector_grade_controls_tests.cc) and needs no .cc of
    # its own; InspectorView.mm is AppKit-only, same as the rest of this
    # block.
    src/InspectorView.mm
    # F2.4 -- chat panel (ROADMAP.md). ChatLlmClient.cc/ChatSession.cc/
    # McpLiveBackend.cc are plain C++ and already part of cutmachine_model
    # above (see tests/chat_llm_client_tests.cc, tests/chat_session_tests.cc);
    # this is only the AppKit view that wires them to the live document.
    src/ChatPanelView.mm
)

add_executable(cutmachine ${CUTMACHINE_APP_SOURCES})
add_dependencies(cutmachine cutmachine_ui_icons)
target_include_directories(cutmachine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cutmachine PRIVATE cutmachine_model)
target_compile_options(cutmachine PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)
target_compile_definitions(cutmachine PRIVATE
    CUTMACHINE_SHADER_PATH="${SHADER_RUNTIME_PATH}"
    CUTMACHINE_ICON_DIR="${CUTMACHINE_ICON_OUTPUT_DIR}"
)
target_link_libraries(cutmachine PRIVATE
    PkgConfig::FFMPEG
    "-framework AppKit"
    "-framework AVFAudio"
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Security"
    "-framework UniformTypeIdentifiers"
)

# A regular macOS application for Finder/Dock launches. The plain `cutmachine`
# executable remains available for the CLI commands and automated workflows.
add_executable(cutmachine_app MACOSX_BUNDLE
    ${CUTMACHINE_APP_SOURCES}
    src/shader.metal
    ${CUTMACHINE_ICON_PNGS}
    assets/icons/LICENSE
    ${OCIO_LICENSE_RESOURCE}
)
set_source_files_properties(src/shader.metal PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)
set_source_files_properties(${CUTMACHINE_ICON_PNGS} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources/icons
)
set_source_files_properties(assets/icons/LICENSE PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources/icons
)
set_source_files_properties(${OCIO_LICENSE_RESOURCE} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources/licenses
)
add_dependencies(cutmachine_app cutmachine_ui_icons)
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
    CUTMACHINE_ICON_DIR="${CUTMACHINE_ICON_OUTPUT_DIR}"
)
target_link_libraries(cutmachine_app PRIVATE
    PkgConfig::FFMPEG
    "-framework AppKit"
    "-framework AVFAudio"
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Security"
    "-framework UniformTypeIdentifiers"
)

# DEV-2026-08 -- keep the build-and-launch path in CMake so every generator
# opens the exact bundle it just produced, including non-default build dirs.
add_custom_target(run-app
    COMMAND /usr/bin/open "$<TARGET_BUNDLE_DIR:cutmachine_app>"
    DEPENDS cutmachine_app
    COMMENT "Launching CUTMACHINE.app"
    VERBATIM
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

    add_executable(cutmachine_timeline_stats_tests
                   tests/timeline_stats_tests.cc)
    target_link_libraries(cutmachine_timeline_stats_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_timeline_stats_tests
             COMMAND cutmachine_timeline_stats_tests)

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

    add_executable(cutmachine_color_effects_tests tests/color_effects_tests.cc)
    target_link_libraries(cutmachine_color_effects_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_color_effects_tests
             COMMAND cutmachine_color_effects_tests)

    # F2.2 -- Inspector (ROADMAP.md). InspectorGradeControls.h is plain
    # C++, no AppKit dependency -- like UiTheme.h/PanelLayout.h above -- so
    # this builds and runs on a plain Linux host too.
    add_executable(cutmachine_inspector_grade_controls_tests
                   tests/inspector_grade_controls_tests.cc)
    target_link_libraries(cutmachine_inspector_grade_controls_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_inspector_grade_controls_tests
             COMMAND cutmachine_inspector_grade_controls_tests)

    add_executable(cutmachine_project_recovery_tests
                   tests/project_recovery_tests.cc)
    target_link_libraries(cutmachine_project_recovery_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_recovery_tests
             COMMAND cutmachine_project_recovery_tests)

    add_executable(cutmachine_project_tests tests/project_tests.cc)
    target_link_libraries(cutmachine_project_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_tests COMMAND cutmachine_project_tests)

    add_executable(cutmachine_interview_short_tests
                   tests/interview_short_tests.cc)
    target_link_libraries(cutmachine_interview_short_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_interview_short_tests
             COMMAND cutmachine_interview_short_tests)

    add_executable(cutmachine_project_storage_tests
                   tests/project_storage_tests.cc)
    target_link_libraries(cutmachine_project_storage_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_storage_tests
             COMMAND cutmachine_project_storage_tests)

    add_executable(cutmachine_sequence_format_tests
        tests/sequence_format_tests.cc)
    target_link_libraries(cutmachine_sequence_format_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_sequence_format_tests
             COMMAND cutmachine_sequence_format_tests)

    add_executable(cutmachine_project_bin_tests tests/project_bin_tests.cc)
    target_link_libraries(cutmachine_project_bin_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_bin_tests COMMAND cutmachine_project_bin_tests)

    add_executable(cutmachine_export_tests tests/export_tests.cc)
    target_link_libraries(cutmachine_export_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_export_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
    )
    add_test(NAME cutmachine_export_tests COMMAND cutmachine_export_tests)

    add_executable(cutmachine_ingest_tests
        tests/ingest_tests.cc
        src/MediaSource.mm
    )
    target_link_libraries(cutmachine_ingest_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_ingest_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_ingest_tests COMMAND cutmachine_ingest_tests)

    add_executable(cutmachine_resolve_export_tests
        tests/resolve_export_tests.cc)
    target_link_libraries(cutmachine_resolve_export_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_resolve_export_tests
             COMMAND cutmachine_resolve_export_tests)

    add_executable(cutmachine_resolve_import_tests
        tests/resolve_import_tests.cc
        src/MediaSource.mm
    )
    target_link_libraries(cutmachine_resolve_import_tests
                          PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_resolve_import_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_resolve_import_tests
             COMMAND cutmachine_resolve_import_tests)

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

    add_executable(cutmachine_transcription_tests tests/transcription_tests.cc)
    target_link_libraries(cutmachine_transcription_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_transcription_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_transcription_tests
             COMMAND cutmachine_transcription_tests)

    add_executable(cutmachine_frame_capture_tests tests/frame_capture_tests.cc)
    target_link_libraries(cutmachine_frame_capture_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_frame_capture_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_frame_capture_tests
             COMMAND cutmachine_frame_capture_tests)

    add_executable(cutmachine_shot_quality_tests tests/shot_quality_tests.cc)
    target_link_libraries(cutmachine_shot_quality_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_shot_quality_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_shot_quality_tests
             COMMAND cutmachine_shot_quality_tests)

    add_executable(cutmachine_document_delta_tests
        tests/document_delta_tests.cc)
    target_link_libraries(cutmachine_document_delta_tests
        PRIVATE cutmachine_model)
    add_test(NAME cutmachine_document_delta_tests
             COMMAND cutmachine_document_delta_tests)

    add_executable(cutmachine_transcript_alignment_tests
        tests/transcript_alignment_tests.cc)
    target_link_libraries(cutmachine_transcript_alignment_tests
        PRIVATE cutmachine_model)
    add_test(NAME cutmachine_transcript_alignment_tests
             COMMAND cutmachine_transcript_alignment_tests)

    add_executable(cutmachine_source_address_tests
        tests/source_address_tests.cc)
    target_link_libraries(cutmachine_source_address_tests
        PRIVATE cutmachine_model)
    add_test(NAME cutmachine_source_address_tests
             COMMAND cutmachine_source_address_tests)

    add_executable(cutmachine_pause_tightening_tests
        tests/pause_tightening_tests.cc)
    target_link_libraries(cutmachine_pause_tightening_tests
        PRIVATE cutmachine_model)
    add_test(NAME cutmachine_pause_tightening_tests
             COMMAND cutmachine_pause_tightening_tests)

    add_executable(cutmachine_speech_onset_tests tests/speech_onset_tests.cc)
    target_link_libraries(cutmachine_speech_onset_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_speech_onset_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_speech_onset_tests
             COMMAND cutmachine_speech_onset_tests)

    add_executable(cutmachine_subtitle_tests tests/subtitle_tests.cc)
    target_link_libraries(cutmachine_subtitle_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_subtitle_tests COMMAND cutmachine_subtitle_tests)

    add_executable(cutmachine_thumbnail_tests tests/thumbnail_tests.cc)
    target_link_libraries(cutmachine_thumbnail_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_thumbnail_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_thumbnail_tests COMMAND cutmachine_thumbnail_tests)

    add_executable(cutmachine_fft_tests tests/fft_tests.cc)
    target_include_directories(cutmachine_fft_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_fft_tests COMMAND cutmachine_fft_tests)

    # F2.1 -- design system (ROADMAP.md). UiTheme.h and PanelLayout.h are
    # plain C++, no AppKit dependency, so -- like Fft.h above -- these build
    # and run on a plain Linux host.
    add_executable(cutmachine_ui_theme_tests tests/ui_theme_tests.cc)
    target_include_directories(cutmachine_ui_theme_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_ui_theme_tests COMMAND cutmachine_ui_theme_tests)

    add_executable(cutmachine_panel_layout_tests tests/panel_layout_tests.cc)
    target_include_directories(cutmachine_panel_layout_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_panel_layout_tests
             COMMAND cutmachine_panel_layout_tests)

    # F2.5 -- playback transport (ROADMAP.md). TransportBar.h and Timecode.h
    # are plain C++, no AppKit dependency, so -- like UiTheme.h/PanelLayout.h
    # above -- these build and run on a plain Linux host.
    add_executable(cutmachine_transport_bar_tests
                   tests/transport_bar_tests.cc)
    target_include_directories(cutmachine_transport_bar_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_transport_bar_tests
             COMMAND cutmachine_transport_bar_tests)

    add_executable(cutmachine_timecode_tests tests/timecode_tests.cc)
    target_include_directories(cutmachine_timecode_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_timecode_tests COMMAND cutmachine_timecode_tests)

    # F2.3 -- Media panel view-model (ROADMAP.md). MediaPanelModel.h depends
    # on Document.h/Operations.h (GenerateUlid needs Ulid.cc), so unlike
    # UiTheme.h/PanelLayout.h above it links cutmachine_model, but it is
    # still plain C++ with no AppKit dependency and runs on a plain Linux
    # host the same way cutmachine_project_bin_tests does.
    add_executable(cutmachine_media_panel_model_tests
                   tests/media_panel_model_tests.cc)
    target_link_libraries(cutmachine_media_panel_model_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_media_panel_model_tests
             COMMAND cutmachine_media_panel_model_tests)

    add_executable(cutmachine_beat_detection_tests
                   tests/beat_detection_tests.cc)
    target_link_libraries(cutmachine_beat_detection_tests
                          PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_beat_detection_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_beat_detection_tests
             COMMAND cutmachine_beat_detection_tests)

    add_executable(cutmachine_relink_tests tests/relink_tests.cc)
    target_link_libraries(cutmachine_relink_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_relink_tests COMMAND cutmachine_relink_tests)

    # F2.4 -- chat panel (ROADMAP.md). Both pure C++ (Json/McpTools/
    # ChatLlmClient/ChatSession only), no AppKit -- see each file's own
    # header comment for what they prove, in particular that a chat-driven
    # tool call dispatches through the real McpToolRegistry::Call and
    # EditLog::Apply, not a parallel mutation path.
    add_executable(cutmachine_chat_llm_client_tests
                   tests/chat_llm_client_tests.cc)
    target_link_libraries(cutmachine_chat_llm_client_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_chat_llm_client_tests
             COMMAND cutmachine_chat_llm_client_tests)

    add_executable(cutmachine_chat_session_tests tests/chat_session_tests.cc)
    target_link_libraries(cutmachine_chat_session_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_chat_session_tests
             COMMAND cutmachine_chat_session_tests)

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

    # Native AppKit/Metal smoke test. It launches the real AppDelegate against
    # a disposable project and exercises target/action wiring in-process, so
    # it needs a macOS WindowServer but no Accessibility permission.
    add_executable(cutmachine_ui_smoke_tests ${CUTMACHINE_APP_SOURCES})
    add_dependencies(cutmachine_ui_smoke_tests cutmachine_ui_icons)
    target_include_directories(cutmachine_ui_smoke_tests
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(cutmachine_ui_smoke_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(cutmachine_ui_smoke_tests PRIVATE
        CUTMACHINE_UI_SMOKE_TEST=1
        CUTMACHINE_SHADER_PATH="${SHADER_RUNTIME_PATH}"
        CUTMACHINE_ICON_DIR="${CUTMACHINE_ICON_OUTPUT_DIR}"
    )
    target_link_libraries(cutmachine_ui_smoke_tests PRIVATE
        cutmachine_model
        PkgConfig::FFMPEG
        "-framework AppKit"
        "-framework AVFAudio"
        "-framework Foundation"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Security"
        "-framework UniformTypeIdentifiers"
    )
    add_test(NAME cutmachine_ui_smoke_tests
             COMMAND cutmachine_ui_smoke_tests --ui-smoke)
    set_tests_properties(cutmachine_ui_smoke_tests PROPERTIES TIMEOUT 30)

    add_test(
        NAME cutmachine_sidecar_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest sidecar.test_sidecar
    )
    set_tests_properties(cutmachine_sidecar_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    # QC-2026-08 -- these regression suites were delivered as detached
    # artifacts under "Test copilot/". Register them here so exact-time,
    # stateful edit-log and storage-failure properties run with the normal
    # local CTest gate instead of existing only as unaudited source files.
    add_executable(cutmachine_rational_time_property_tests
                   tests/rational_time_property_tests.cc)
    target_link_libraries(cutmachine_rational_time_property_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_rational_time_property_tests
             COMMAND cutmachine_rational_time_property_tests)

    add_executable(cutmachine_edit_sequence_tests
                   tests/edit_sequence_tests.cc)
    target_link_libraries(cutmachine_edit_sequence_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_edit_sequence_tests
             COMMAND cutmachine_edit_sequence_tests)

    add_executable(cutmachine_storage_fault_injection_tests
                   tests/storage_fault_injection_tests.cc)
    target_link_libraries(cutmachine_storage_fault_injection_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_storage_fault_injection_tests
             COMMAND cutmachine_storage_fault_injection_tests)

    set_tests_properties(
        cutmachine_rational_time_property_tests
        cutmachine_edit_sequence_tests
        cutmachine_storage_fault_injection_tests
        PROPERTIES LABELS "portable;regression"
    )
    set_tests_properties(
        cutmachine_storage_fault_injection_tests
        cutmachine_project_recovery_tests
        cutmachine_project_storage_tests
        PROPERTIES LABELS "storage"
    )

    # The Resolve plugin's pure core is Lua, so its tests run under fuscript,
    # the Lua 5.1 interpreter DaVinci Resolve ships. Registered only where
    # Resolve is installed: this is a tool test, not a reason to fail a build
    # on a machine without it.
    #
    # fuscript ALWAYS exits 0 -- on os.exit(1) and on an uncaught Lua error
    # alike -- so the verdict has to come from its output. Without
    # PASS_REGULAR_EXPRESSION this test would pass while asserting nothing.
    set(RESOLVE_FUSCRIPT "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries/Fusion/fuscript")
    if(EXISTS "${RESOLVE_FUSCRIPT}")
        add_test(
            NAME cutmachine_resolve_plugin_tests
            COMMAND "${RESOLVE_FUSCRIPT}" -l lua
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/resolve-plugin/tests/test_cutmachine_lua.lua"
        )
        set_tests_properties(cutmachine_resolve_plugin_tests PROPERTIES
            PASS_REGULAR_EXPRESSION "plugin resolve : tests passés"
            TIMEOUT 30
        )
    endif()
endif()
