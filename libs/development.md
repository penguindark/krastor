# libs/ — vendored third-party libraries

Development doc for this folder (see `../project.md` section 4.3).

## Purpose
Source of third-party libraries vendored into the repo (small, permissive-licensed,
must compile with MinGW GCC and MSVC on Windows and GCC on Linux).

## Current state (Phase 5, 2026-10-29)

| Library | License | Form | Where |
|---|---|---|---|
| nlohmann/json 3.12 | MIT | 1 header | `nlohmann/json.hpp` — config + LLM JSON |
| GLFW 3.4 | zlib | full source (built via `add_subdirectory`) | `glfw/` — window + GL context |
| glad (OpenGL 3.3 core) | MIT | generated 2 files | `glad/` (`include/glad/glad.h`, `src/glad.c`) |
| Nuklear 4.13 | zlib | 1 header + glfw_gl3 backend | `nuklear/nuklear.h`, `nuklear/nuklear_glfw_gl3.h` |
| miniaudio 0.11.22 | public domain (unlicense) | 1 header + `miniaudio_impl.c` | `miniaudio.h` — speaker playback + mic capture |
| stb_image_write 1.16 | public domain / MIT | 1 header | `stb_image_write.h` — PNG encode (clipboard/dropped images → base64 data URLs) |
| stb_image 2.30 | public domain / MIT | 1 header | `stb_image.h` - image decode (drag & drop file attachments) |
| LunaSVG 3.5.0 | MIT | **git submodule** (`git submodule add https://github.com/sammycage/lunasvg libs/lunasvg`, pinned tag `v3.5.0`) | `lunasvg/` - SVG rasterizer for UI icons; built as static lib via `add_subdirectory`, used only through `src/icons.cpp` |
| sherpa-onnx 1.13.8 (C API) | Apache-2.0 | prebuilt win-x64 shared: header committed, `*.dll`/import lib git-ignored | `sherpa-onnx/` — fetch binaries with `tools/download_deps.py` |

## Dependency policy (project.md section 7)

| Kind | How it enters git |
|---|---|
| Library with full sources | **git submodule** - only the URL + pinned commit SHA live in the repo (`lunasvg`; `glfw` is a candidate for migration) |
| Prebuilt binaries | never in git - fetched by `tools/download_deps.py` (`sherpa-onnx`) |
| Single-header libs | vendored directly (1-2 files, a submodule would cost more than it saves) |

Fresh clones need `git clone --recursive` (or the build scripts auto-run
`git submodule update --init --recursive` when `libs/lunasvg/CMakeLists.txt`
is missing). CMake aborts with the same hint when the submodule is absent.

LunaSVG notes:
- v3.5.0 vendors plutovg inside its own repo - no nested submodule needed.
- Built static with `LUNASVG_BUILD_EXAMPLES=OFF`,
  `LUNASVG_DISABLE_LOAD_SYSTEM_FONTS=ON`, `LUNASVG_DISABLE_EXTERNAL_RESOURCES=ON`:
  Krastor reads the `.svg` files itself and passes bytes to
  `Document::loadFromData`, so the library never touches the filesystem.
- Chosen over nanosvg (the original Phase 5 plan): nanosvg is unmaintained and
  its rasterizer only fills paths (no `stroke`), which rules out stroke-based
  icon sets like Lucide. LunaSVG is MIT, actively maintained, C++17, compiles
  clean with MinGW GCC and MSVC.

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

Planned for later Phase 5: md4c. libcurl on Linux is
found via `find_package(CURL)`, not vendored. (nanosvg from the original plan
was replaced by LunaSVG; stb_image is already in.)

## Conventions
- One subfolder per library, pinned version, keep upstream `LICENSE` file.
- Record version + source URL here when adding a library.
