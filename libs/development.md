# libs/ — vendored third-party libraries

Development doc for this folder (see `../project.md` section 4.3).

## Purpose
Source of third-party libraries vendored into the repo (small, permissive-licensed,
must compile with MinGW GCC and MSVC on Windows and GCC on Linux).

## Current state (Phase 1)
Empty (`.gitkeep`). Populated in Phase 5 with the locked stack:

| Library | License | Form |
|---|---|---|
| GLFW | zlib | prebuilt lib + headers |
| glad (OpenGL 3.3 core) | MIT | 2 files |
| Nuklear | MIT | 1 header |
| nanosvg | zlib | 2 files |
| stb_image | MIT/UNLICENSE | headers |
| md4c | MIT | 2 C files |
| nlohmann/json | MIT | 1 header |
| miniaudio | public domain/MIT | 1 header |

libcurl will be linked from a system/prebuilt install, not vendored here.

## Conventions
- One subfolder per library, pinned version, keep upstream `LICENSE` file.
- Record version + source URL here when adding a library.
