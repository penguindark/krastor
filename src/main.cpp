// Krastor — LLM agent with voice input/output and real-time graphics interface.
// Phase 1 skeleton: verifies the dual-toolchain C++20 build pipeline.
// See project.md (master description) and src/development.md (this folder).

#include <iostream>
#include <string>

namespace {

// Human-readable compiler identification, useful to confirm which toolchain
// produced the running binary (MinGW GCC vs MSVC).
constexpr const char* compilerName()
{
#if defined(_MSC_VER)
    return "MSVC";
#elif defined(__GNUC__)
    return "GCC (MinGW)";
#else
    return "Unknown";
#endif
}

constexpr const char* cxxStandard()
{
    // _MSVC_LANG is the reliable macro under MSVC (it ignores /Zc:__cplusplus).
#if defined(_MSVC_LANG)
    return _MSVC_LANG >= 202002L ? "C++20" : "pre-C++20";
#elif __cplusplus >= 202002L
    return "C++20";
#else
    return "pre-C++20";
#endif
}

} // namespace

int main()
{
    std::cout << "Krastor v0.1.0 (Phase 1 skeleton)\n"
              << "compiler: " << compilerName() << "\n"
              << "standard: " << cxxStandard() << "\n";
    return 0;
}
