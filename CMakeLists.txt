# Krastor — root build file.
# C++20, dual Windows toolchain (MinGW GCC + MSVC) and Linux.
# Build through CMakePresets (see project.md section 7).
cmake_minimum_required(VERSION 3.25)

project(krastor
    VERSION 0.1.0
    DESCRIPTION "LLM agent with voice input/output and real-time graphics interface"
    LANGUAGES CXX
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

add_executable(krastor
    src/main.cpp
    src/config.cpp
    src/conversation.cpp
    src/llm_client.cpp
    src/voices.cpp
)

target_compile_features(krastor PRIVATE cxx_std_20)

# Vendored single-header libraries (see libs/development.md).
target_include_directories(krastor PRIVATE "${CMAKE_SOURCE_DIR}/libs")

if(WIN32)
    # HTTPS via WinHTTP (built into Windows) — identical behavior on MinGW and MSVC.
    target_sources(krastor PRIVATE src/http_client_winhttp.cpp)
    target_link_libraries(krastor PRIVATE winhttp)
else()
    target_sources(krastor PRIVATE src/http_client_curl.cpp)
    find_package(CURL REQUIRED)
    target_link_libraries(krastor PRIVATE CURL::libcurl)
endif()

if(MSVC)
    target_compile_options(krastor PRIVATE /W4 /utf-8)
else()
    target_compile_options(krastor PRIVATE -Wall -Wextra)
endif()
