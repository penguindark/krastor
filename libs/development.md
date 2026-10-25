# libs/ — vendored third-party libraries

Development doc for this folder (see `../project.md` section 4.3).

## Purpose
Source of third-party libraries vendored into the repo (small, permissive-licensed,
must compile with MinGW GCC and MSVC on Windows and GCC on Linux).

## Current state (Phase 4, 2026-10-26)

| Library | License | Form | Where |
|---|---|---|---|
| nlohmann/json 3.12 | MIT | 1 header | `nlohmann/json.hpp` — config + LLM JSON |
| GLFW 3.4 | zlib | full source (built via `add_subdirectory`) | `glfw/` — window + GL context |
| glad (OpenGL 3.3 core) | MIT | generated 2 files | `glad/` (`include/glad/glad.h`, `src/glad.c`) |
| Nuklear 4.13 | zlib | 1 header + glfw_gl3 backend | `nuklear/nuklear.h`, `nuklear/nuklear_glfw_gl3.h` |
| miniaudio 0.11.22 | public domain (unlicense) | 1 header + `miniaudio_impl.c` | `miniaudio.h` — speaker playback + mic capture |
| sherpa-onnx 1.13.8 (C API) | Apache-2.0 | prebuilt win-x64 shared: header committed, `*.dll`/import lib git-ignored | `sherpa-onnx/` — fetch binaries with `tools/download_deps.py` |

sherpa-onnx notes:
- Source: GitHub release `v1.13.8` asset `sherpa-onnx-v1.13.8-win-x64-shared-MD-Release.tar.bz2`
  (k2-fsa/sherpa-onnx). Only `include/sherpa-onnx/c-api/c-api.h`, `lib/sherpa-onnx-c-api.*`,
  `lib/onnxruntime*.dll|lib` are kept; CMake copies the DLLs next to `bin/krastor.exe`.
- **Pinned to 1.13.8 on purpose**: v2.0.0 ignores `data_dir` (bundled espeak-ng) for
  VITS/Piper models, which breaks our Piper voices. Only the extern-"C" API is used,
  so the MSVC-built import lib also links with MinGW.
- miniaudio is pinned to the 0.11.22 release header (master renamed buffer/decoder APIs).

Removed: `httplib` (cpp-httplib 0.63) — its HTTPS transport requires OpenSSL,
which the compiler-only MinGW install lacks; replaced by WinHTTP (Windows) and
libcurl (Linux) in `src/http_client_*.cpp`.

Planned for later Phase 5: nanosvg, stb_image, md4c. libcurl on Linux is
found via `find_package(CURL)`, not vendored.

## Conventions
- One subfolder per library, pinned version, keep upstream `LICENSE` file.
- Record version + source URL here when adding a library.
