#pragma once
#include "../defines.h"

#include <string_view>

// Fire-and-forget sound effects. Deliberately minimal -- the engine has no
// mixer, no music streaming and no 3D audio, just "play this short clip
// now":
//
//   audio_play_sound("ding"); // assets/sounds/ding.wav
//
// A name resolves to assets/sounds/<name>.wav, the same name -> path
// convention TextureSystem uses for assets/textures/<name>.png. The file
// must be uncompressed PCM, 16-bit, mono or stereo, at any sample rate.
// It's read from disk the first time it's played and kept in memory after
// that.
//
// Playback happens on a background thread (through ALSA, which PipeWire
// and PulseAudio both serve; on Windows, winmm's PlaySound), so this call
// never blocks the frame. A new
// play cuts off whatever is still sounding and starts fresh -- the newest
// cue wins, nothing mixes or queues. Fine for the occasional cue this
// exists for (back-to-back title cards each get their own ding), wrong for
// anything denser.
// The device is opened lazily on the first play and released when the
// engine library unloads, so there's nothing to initialize.
//
// Failures (missing/malformed file, no audio device) are logged once per
// sound and otherwise silent: a game should never stop because a cue
// couldn't play.
KAPI void audio_play_sound(std::string_view name);

// Background music: one looping track at a time, alongside (never cut off
// by) audio_play_sound()'s cues.
//
//   audio_play_music("chapter1"); // assets/sounds/chapter1.mp3, looped
//
// A name resolves to assets/sounds/<name>.mp3. The file is streamed and
// decoded as it plays (libmpg123 on Linux, MCI on Windows) rather than
// loaded whole, so starting a track never stalls the frame.
//
// Asking for the track already playing is a no-op -- it carries on rather
// than restarting, so callers can just state the track they want whenever
// it might have changed. Asking for a different one fades the current
// track out first (Linux; Windows cuts straight over), then starts the new
// one from the top. An empty name, or audio_stop_music(), fades to silence.
//
// The music stream is opened separately from the cue stream, so the two
// are mixed by the sound server (PipeWire/PulseAudio, or ALSA's dmix) on
// Linux and by Windows itself on Windows -- the engine still has no mixer
// of its own. Failures are logged once per track and otherwise silent,
// same as audio_play_sound().
KAPI void audio_play_music(std::string_view name);
KAPI void audio_stop_music();
