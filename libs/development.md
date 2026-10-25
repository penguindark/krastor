# libs/ — vendored third-party libraries

Development doc for this folder (see `../project.md` section 4.3).

## Purpose
Source of third-party libraries vendored into the repo (small, permissive-licensed,
must compile with MinGW GCC and MSVC on Windows and GCC on Linux).

## Current state (Phase 5 foundation, 2026-10-26)

| Library | License | Form | Where |
|---|---|---|---|
| nlohmann/json 3.12 | MIT | 1 header | `nlohmann/json.hpp` — config + LLM JSON |
| GLFW 3.4 | zlib | full source (built via `add_subdirectory`) | `glfw/` — window + GL context |
| glad (OpenGL 3.3 core) | MIT | generated 2 files | `glad/` (`include/glad/glad.h`, `src/glad.c`) |
| Nuklear 4.13 | zlib | 1 header + glfw_gl3 backend | `nuklear/nuklear.h`, `nuklear/nuklear_glfw_gl3.h` |

Removed: `httplib` (cpp-httplib 0.63) — its HTTPS transport requires OpenSSL,
which the compiler-only MinGW install lacks; replaced by WinHTTP (Windows) and
libcurl (Linux) in `src/http_client_*.cpp`.

Planned for later Phase 5: nanosvg, stb_image, md4c, miniaudio. libcurl on Linux is
found via `find_package(CURL)`, not vendored.

## Conventions
- One subfolder per library, pinned version, keep upstream `LICENSE` file.
- Record version + source URL here when adding a library.
