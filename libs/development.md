# libs/ — vendored third-party libraries

Development doc for this folder (see `../project.md` section 4.3).

## Purpose
Source of third-party libraries vendored into the repo (small, permissive-licensed,
must compile with MinGW GCC and MSVC on Windows and GCC on Linux).

## Current state (Phase 3, 2026-10-26)

| Library | License | Form | Where |
|---|---|---|---|
| nlohmann/json 3.12 | MIT | 1 header | `nlohmann/json.hpp` — config + LLM JSON |

Removed: `httplib` (cpp-httplib 0.63) — its HTTPS transport requires OpenSSL,
which the compiler-only MinGW install lacks; replaced by WinHTTP (Windows) and
libcurl (Linux) in `src/http_client_*.cpp`.

Planned for Phase 5 (see project.md 4.3): GLFW, glad, Nuklear, nanosvg, stb_image,
md4c, miniaudio. libcurl on Linux is found via `find_package(CURL)`, not vendored.

## Conventions
- One subfolder per library, pinned version, keep upstream `LICENSE` file.
- Record version + source URL here when adding a library.
