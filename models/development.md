# models/ — local AI models (folder is git-ignored)

Development doc for this folder (see `../project.md` section 6).

## Purpose
Local STT and TTS models (English + Italian). The folder itself is **git-ignored**;
weights never enter the repository.

## Policy
- One subfolder per model, named `<role>-<name>`:
  e.g. `stt-whisper-large-v3/`, `tts-piper-it-it-riccardo/`.
- Each model subfolder contains a `README.md` recording: source URL, license,
  file list, intended use. (The README is documentation, not weight; commit it
  with an explicit `!models/*/README.md` gitignore exception if we want it tracked.)
- Models are downloaded by scripts in `../tools` (Phase 2) so setup is reproducible.

## Current state (Phase 1)
Folder created, empty. Candidates to evaluate in Phase 2:
- STT: whisper.cpp, sherpa-onnx (Whisper / multilingual Zipformer)
- TTS: Piper (native it_IT + en_US voices), sherpa-onnx VITS, Kokoro
