# Build glue for the two audio-DSP git submodules (neither ships CMake):
#   libs/rnnoise  (xiph RNNoise, BSD-3)  - microphone noise suppression (48 kHz)
#   libs/speexdsp (xiph SpeexDSP, BSD-3) - acoustic echo canceller + resampler
# The submodule sources are compiled as-is; only this file and the hand-written
# speexdsp_config/config.h stand in for the upstream autotools build. Krastor
# touches them only through src/audio_fx.cpp (see libs/development.md).

# RNNoise model weights are generated C tables, not kept in the git repo;
# fetch them first with:  .venv\Scripts\python tools\download_deps.py
set(RNNOISE_DATA "${CMAKE_SOURCE_DIR}/libs/rnnoise-data/src")
if(NOT EXISTS "${RNNOISE_DATA}/rnnoise_data.c")
    message(FATAL_ERROR
        "RNNoise model data missing in libs/rnnoise-data/src - "
        "run: .venv\\Scripts\\python tools\\download_deps.py")
endif()

# Portable float build, no x86 runtime CPU dispatch (the default nnet path
# is plenty for one 48 kHz mono stream and keeps both toolchains simple).
add_library(rnnoise STATIC
    "${CMAKE_SOURCE_DIR}/libs/rnnoise/src/denoise.c"
    "${CMAKE_SOURCE_DIR}/libs/rnnoise/src/rnn.c"
    "${CMAKE_SOURCE_DIR}/libs/rnnoise/src/pitch.c"
    "${CMAKE_SOURCE_DIR}/libs/rnnoise/src/kiss_fft.c"
    "${CMAKE_SOURCE_DIR}/libs/rnnoise/src/celt_lpc.c"
    "${CMAKE_SOURCE_DIR}/libs/rnnoise/src/nnet.c"
    "${CMAKE_SOURCE_DIR}/libs/rnnoise/src/nnet_default.c"
    "${CMAKE_SOURCE_DIR}/libs/rnnoise/src/parse_lpcnet_weights.c"
    "${CMAKE_SOURCE_DIR}/libs/rnnoise/src/rnnoise_tables.c"
    "${RNNOISE_DATA}/rnnoise_data.c"
)
target_include_directories(rnnoise
    PUBLIC  "${CMAKE_SOURCE_DIR}/libs/rnnoise/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/libs/rnnoise/src" "${RNNOISE_DATA}"
)

# Floating-point AEC (mdf.c) + smallft FFT + the streaming resampler.
# HAVE_CONFIG_H points at our own minimal config.h (speexdsp_config/) so the
# sources skip autotools; mirrors win32/config.h minus the MSVC-only bits.
add_library(speexdsp STATIC
    "${CMAKE_SOURCE_DIR}/libs/speexdsp/libspeexdsp/mdf.c"
    "${CMAKE_SOURCE_DIR}/libs/speexdsp/libspeexdsp/fftwrap.c"
    "${CMAKE_SOURCE_DIR}/libs/speexdsp/libspeexdsp/smallft.c"
    "${CMAKE_SOURCE_DIR}/libs/speexdsp/libspeexdsp/filterbank.c"
    "${CMAKE_SOURCE_DIR}/libs/speexdsp/libspeexdsp/buffer.c"
    "${CMAKE_SOURCE_DIR}/libs/speexdsp/libspeexdsp/resample.c"
)
target_include_directories(speexdsp
    PUBLIC  "${CMAKE_SOURCE_DIR}/libs/speexdsp/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/libs/speexdsp/libspeexdsp"
            "${CMAKE_CURRENT_SOURCE_DIR}/speexdsp_config"
)
target_compile_definitions(speexdsp PRIVATE HAVE_CONFIG_H)
if(NOT WIN32)
    target_link_libraries(speexdsp PRIVATE m)
    target_link_libraries(rnnoise PRIVATE m)
endif()
