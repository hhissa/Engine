#pragma once
// Controller mode: the sculpting tool with no Qt at all -- the engine's own
// window, driven entirely by a gamepad. Started by `--controller` (see
// main.cpp). Must never include a Qt header.

// Runs the engine's own application loop in controller mode until the
// window closes. `stamps_path` (may be null) is a .stamps file to load at
// startup. Returns the process exit code.
int run_controller_mode(const char *stamps_path);
