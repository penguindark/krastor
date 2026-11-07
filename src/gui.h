// Krastor — GUI entry point (Phase 5 foundation).
// Opens a GLFW window with a Nuklear UI: a gear icon that opens a settings modal
// for the LLM engine URL, TTS voice and other parameters. See src/development.md.

#pragma once

namespace krastor {

struct Config;
class VoiceRegistry;

// Runs the GUI event loop until the window is closed.
// Edits settings into `cfg` and persists them to config.json on Save.
// Returns the process exit code.
int run_gui(Config& cfg, VoiceRegistry& voices);

// Headless Nuklear self-test (--gui-test): fullscreen base window + modal
// stacking - clicking the base window must never make the modal read-only.
int run_gui_stack_test();

} // namespace krastor
