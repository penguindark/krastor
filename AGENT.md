# AGENT.md — Base instructions & index

Read this first. It is the entry point for any AI agent or developer working on this repo.

## Source of truth
- **`project.md`** — what the project is, architecture, requirements, roadmap.
- **`development_log.md`** - chronological log of all development. Append a dated entry
  after every working session.

## Working rules
1. All documentation, code comments and logs are in **English**.
2. Language: **C++20**. Windows code must compile with **both** MinGW GCC
   (`C:\mingw64`) and MSVC (default: VS 2026). Target platforms: Windows 10, Linux.
3. Build only through CMake presets (`CMakePresets.json`) or `scripts/build.ps1` /
   `scripts/build.sh`. Intermediates go to `obj/`, executables to `bin/`.
4. Python: always `C:\Python313` through `.venv` — never the system PATH python.
5. Never commit models, binaries, build outputs or `.venv` (see `.gitignore`).
   Each model lives in its own `models/<role>-<name>/` subfolder with a README
   (source, license, files). Full-source libraries enter as **git submodules**
   (only URL + pinned SHA in the repo, see `libs/development.md`): clone with
   `git clone --recursive` or let the build scripts auto-init them.
6. Every tracked folder has a `development.md` — **every commit must update the
   `development.md` of every folder it touches** (src, libs, data, tools, scripts...).
7. Keep `project.md` and `development_log.md` updated with every commit that changes
   features, architecture, requirements or tooling: append a dated entry to
   `development_log.md` and reflect the change in `project.md`. Do not batch doc
   updates to the end of a session.

## Index of folder-level development docs
- `src/development.md` — C++ sources
- `data/development.md` — assets (fonts, icons, shaders)
- `models/development.md` — local AI models policy and inventory (folder git-ignored)
- `tools/development.md` — python prototypes/scripts + the agent tool system
  (manifests `tool_*.jsonc`, filetool/webfetch/websearch/python)
- `libs/development.md` — third-party libraries (vendored headers + git submodules)
- `scripts/development.md` — build/dev scripts

## Quick start
```powershell
git submodule update --init --recursive                # first clone only
scripts\build.ps1 -Toolchain msvc2026 -Config Debug   # default MSVC
scripts\build.ps1 -Toolchain mingw   -Config Debug   # MinGW GCC
```
Executable output: `bin\krastor.exe`.
