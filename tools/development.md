# tools/ — python prototypes & scripts

Development doc for this folder (see `../project.md`).

## Purpose
Python prototypes (model evaluation, downloaders, data prep) supporting the C++
product. Never part of the shipped agent.

## Environment
- Virtual env at `../.venv`, created from `C:\Python313\python.exe` (3.13.1).
- Activate: `..\..\.venv\Scripts\Activate.ps1` (from this folder) or call
  `.venv\Scripts\python.exe` explicitly. Never use the PATH python (3.11).
- `.venv` is git-ignored.

## Current state (Phase 1)
Empty (`.gitkeep`). Phase 2 will add STT/TTS evaluation prototypes and model
download scripts here.
