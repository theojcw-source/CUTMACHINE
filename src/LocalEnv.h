#pragma once

// Machine-local settings, shared by every surface (ALPHA-2026-08).
//
// Extracted verbatim from ChatLlmClient.cc, which has read the API key this
// way since F2.4. It moved here the moment a second setting needed the same
// treatment: the Whisper model path (Transcription.h). Both are the same kind
// of value -- machine-local, not project truth. A model path must never enter
// a project file, or reopening the montage on another Mac would carry a path
// that does not exist there (PHILOSOPHY.md: the same project must produce the
// same montage everywhere).
//
// Why a dotenv file and not NSUserDefaults, where UiPreferences.h keeps
// interface state: the headless surfaces need this. `--transcribe` and
// `--mcp-serve` are the same binary, but Cli.cc and the model library are
// deliberately AppKit-free, and a value only reachable through Foundation
// would be invisible to them. A file both a human and a CLI can read is the
// only thing that serves the app, the CLI and the agent at once.
//
// The real environment always wins over the file: LoadDotEnv never overwrites
// a variable that is already set, so a one-off `CUTMACHINE_WHISPER_MODEL=…`
// in front of a command overrides the configured value without editing
// anything.

#include <string>

namespace local_env {

// $CUTMACHINE_ENV_FILE if set, else ~/.config/cutmachine/.env. Missing files
// are not an error: an unconfigured install is a normal state, and every
// caller has to handle an absent value anyway.
void LoadLocalEnvFileIfPresent();

// The path LoadLocalEnvFileIfPresent() reads, for error messages that tell
// the user which file to edit rather than making them guess.
std::string LocalEnvFilePath();

// Loads the file if needed, then reads one variable. Empty when unset.
std::string Value(const char* name);

}  // namespace local_env
