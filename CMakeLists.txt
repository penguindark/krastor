# Krastor — root build file.
# C++20, dual Windows toolchain (MinGW GCC + MSVC) and Linux.
# Build through CMakePresets (see project.md section 7).
cmake_minimum_required(VERSION 3.25)

project(krastor
    VERSION 0.1.0
    DESCRIPTION "LLM agent with voice input/output and real-time graphics interface"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Executables always land in <repo>/bin, regardless of generator/config.
set(KRASTOR_BIN_DIR "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${KRASTOR_BIN_DIR}")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${KRASTOR_BIN_DIR}")
endforeach()

# Third-party graphics stack (see libs/development.md).
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/glfw EXCLUDE_FROM_ALL)

# LunaSVG (git submodule, MIT): SVG rasterizer for the UI icons. Isolated
# behind src/icons.cpp - no other Krastor source includes lunasvg.h. Static
# build, no examples, no system font scan, no external resource loading
# (Krastor reads the .svg files itself and passes the bytes to the parser).
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/libs/lunasvg/CMakeLists.txt")
    message(FATAL_ERROR
        "lunasvg submodule missing in libs/lunasvg - "
        "run: git submodule update --init --recursive")
endif()
set(LUNASVG_BUILD_EXAMPLES        OFF CACHE BOOL "" FORCE)
set(LUNASVG_DISABLE_LOAD_SYSTEM_FONTS ON CACHE BOOL "" FORCE)
set(LUNASVG_DISABLE_EXTERNAL_RESOURCES ON CACHE BOOL "" FORCE)
add_subdirectory(libs/lunasvg EXCLUDE_FROM_ALL)

# Audio input DSP (git submodules, BSD-3): RNNoise (mic noise suppression)
# and SpeexDSP (acoustic echo cancellation + resamplers). Neither ships
# CMakeLists.txt, so libs/audiofx provides the build glue; the libraries are
# touched only through src/audio_fx.cpp (same isolation rule as LunaSVG).
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/libs/rnnoise/include/rnnoise.h" OR
   NOT EXISTS "${CMAKE_SOURCE_DIR}/libs/speexdsp/include/speex/speex_echo.h")
    message(FATAL_ERROR
        "rnnoise/speexdsp submodules missing - "
        "run: git submodule update --init --recursive")
endif()
add_subdirectory(libs/audiofx)

# sherpa-onnx prebuilt C API for local TTS/STT (Windows). The binaries are
# git-ignored; fetch them first with:  .venv\Scripts\python tools\download_deps.py
if(WIN32)
    set(SHERPA_DIR "${CMAKE_SOURCE_DIR}/libs/sherpa-onnx")
    if(NOT EXISTS "${SHERPA_DIR}/lib/sherpa-onnx-c-api.lib")
        message(FATAL_ERROR
            "sherpa-onnx binaries missing in libs/sherpa-onnx/lib — "
            "run: .venv\\Scripts\\python tools\\download_deps.py")
    endif()
    add_library(sherpa-onnx-c-api SHARED IMPORTED GLOBAL)
    set_target_properties(sherpa-onnx-c-api PROPERTIES
        IMPORTED_IMPLIB   "${SHERPA_DIR}/lib/sherpa-onnx-c-api.lib"
        IMPORTED_LOCATION "${SHERPA_DIR}/lib/sherpa-onnx-c-api.dll")
    target_include_directories(sherpa-onnx-c-api INTERFACE "${SHERPA_DIR}/include")
endif()

add_executable(krastor
    src/main.cpp
    src/config.cpp
    src/conversation.cpp
    src/session_store.cpp
    src/compactor.cpp
    src/llm_client.cpp
    src/llm_models.cpp
    src/md_format.cpp
    src/formatters.cpp
    src/highlight.cpp
    src/emoji.cpp
    src/voices.cpp
    src/tts.cpp
    src/tts_text.cpp
    src/stt.cpp
    src/audio_fx.cpp
    src/permissions.cpp
    src/tool_registry.cpp
    src/tool_runner.cpp
    src/tool_loop.cpp
    src/gui.cpp
    src/icons.cpp
    src/clipboard.cpp
    src/media.cpp
    src/effects.cpp
src/key_repeat.cpp
    src/audio_analysis.cpp
    libs/glad/src/glad.c
    libs/miniaudio_impl.c
)

target_compile_features(krastor PRIVATE cxx_std_20)

target_include_directories(krastor PRIVATE
    "${CMAKE_SOURCE_DIR}/libs"
    "${CMAKE_SOURCE_DIR}/libs/nuklear"
    "${CMAKE_SOURCE_DIR}/libs/glad/include"
    "${CMAKE_SOURCE_DIR}/libs/glfw/include"
)

target_link_libraries(krastor PRIVATE glfw lunasvg::lunasvg rnnoise speexdsp)

# --- Agent tools (see src/development.md) ----------------------------------------
# Small standalone exes built into tools/ next to their tool_*.jsonc
# manifests. filetool reuses src/permissions.cpp (glob matcher + policy).
add_executable(filetool tools/filetool/filetool.cpp src/permissions.cpp)
target_compile_features(filetool PRIVATE cxx_std_20)
target_include_directories(filetool PRIVATE "${CMAKE_SOURCE_DIR}/libs")
set_target_properties(filetool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/tools")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(filetool PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_SOURCE_DIR}/tools")
endforeach()

if(WIN32)
    add_executable(webfetch tools/webfetch/webfetch.cpp src/permissions.cpp
                           src/http_client_winhttp.cpp)
    add_executable(websearch tools/websearch/websearch.cpp src/permissions.cpp
                             src/http_client_winhttp.cpp)
else()
    add_executable(webfetch tools/webfetch/webfetch.cpp src/permissions.cpp
                           src/http_client_curl.cpp)
    add_executable(websearch tools/websearch/websearch.cpp src/permissions.cpp
                             src/http_client_curl.cpp)
endif()
target_compile_features(webfetch PRIVATE cxx_std_20)
target_compile_features(websearch PRIVATE cxx_std_20)
target_include_directories(webfetch PRIVATE
    "${CMAKE_SOURCE_DIR}/libs" "${CMAKE_SOURCE_DIR}/src")
target_include_directories(websearch PRIVATE
    "${CMAKE_SOURCE_DIR}/libs" "${CMAKE_SOURCE_DIR}/src")
if(WIN32)
    target_link_libraries(webfetch PRIVATE winhttp)
    target_link_libraries(websearch PRIVATE winhttp)
else()
    target_link_libraries(webfetch PRIVATE CURL::libcurl)
    target_link_libraries(websearch PRIVATE CURL::libcurl)
endif()
set_target_properties(webfetch PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/tools")
set_target_properties(websearch PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/tools")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(webfetch PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_SOURCE_DIR}/tools")
    set_target_properties(websearch PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_SOURCE_DIR}/tools")
endforeach()

if(WIN32)
    # HTTPS via WinHTTP (built into Windows) — identical behavior on MinGW and MSVC.
    target_sources(krastor PRIVATE src/http_client_winhttp.cpp)
    target_link_libraries(krastor PRIVATE winhttp opengl32 sherpa-onnx-c-api)
    # Runtime DLLs must sit next to the executable.
    add_custom_command(TARGET krastor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHERPA_DIR}/lib/sherpa-onnx-c-api.dll"
            "${SHERPA_DIR}/lib/onnxruntime.dll"
            "${SHERPA_DIR}/lib/onnxruntime_providers_shared.dll"
            "$<TARGET_FILE_DIR:krastor>/")
else()
    target_sources(krastor PRIVATE src/http_client_curl.cpp)
    find_package(CURL REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(krastor PRIVATE CURL::libcurl OpenGL::GL)
endif()

if(MSVC)
    target_compile_options(krastor PRIVATE /W4 /utf-8)
else()
    target_compile_options(krastor PRIVATE -Wall -Wextra)
endif()
