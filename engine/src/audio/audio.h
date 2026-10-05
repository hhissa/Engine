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
// and PulseAudio both serve), so this call never blocks the frame. Sounds
// queued while another is still playing wait their turn rather than mix --
// fine for the occasional cue this exists for, wrong for anything denser.
// The device is opened lazily on the first play and released when the
// engine library unloads, so there's nothing to initialize.
//
// Failures (missing/malformed file, no audio device) are logged once per
// sound and otherwise silent: a game should never stop because a cue
// couldn't play.
KAPI void audio_play_sound(std::string_view name);
