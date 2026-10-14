#include "audio.h"

#include "../core/logger.h"

#if KPLATFORM_LINUX
#include <alsa/asoundlib.h>
#include <mpg123.h>
#elif KPLATFORM_WINDOWS
// NOGDI: wingdi.h's `#define ERROR 0` would break KERROR -- see
// platform_win32.cpp.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <windows.h>
#include <mmsystem.h>
#endif

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct SoundClip {
  u32 sample_rate = 0;
  u32 channels = 0;
  std::vector<i16> samples; // interleaved
};

u32 read_u32(const char *bytes) {
  return static_cast<u32>(static_cast<u8>(bytes[0])) |
         static_cast<u32>(static_cast<u8>(bytes[1])) << 8 |
         static_cast<u32>(static_cast<u8>(bytes[2])) << 16 |
         static_cast<u32>(static_cast<u8>(bytes[3])) << 24;
}

u16 read_u16(const char *bytes) {
  return static_cast<u16>(static_cast<u8>(bytes[0]) |
                          static_cast<u8>(bytes[1]) << 8);
}

// Reads a RIFF/WAVE file's "fmt " and "data" chunks, skipping any others
// (LIST, fact, ...) in between -- see audio.h for the formats accepted.
std::optional<SoundClip> load_wav(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    KERROR("Sound '{}' not found.", path);
    return std::nullopt;
  }
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    KERROR("Sound '{}' is not a WAV file.", path);
    return std::nullopt;
  }

  SoundClip clip;
  u16 format = 0;
  u16 bits = 0;
  bool have_format = false;
  size_t offset = 12;
  while (offset + 8 <= bytes.size()) {
    const char *chunk = bytes.data() + offset;
    const u32 size = read_u32(chunk + 4);
    const size_t body = offset + 8;
    if (body + size > bytes.size()) {
      break;
    }
    if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
      format = read_u16(bytes.data() + body);
      clip.channels = read_u16(bytes.data() + body + 2);
      clip.sample_rate = read_u32(bytes.data() + body + 4);
      bits = read_u16(bytes.data() + body + 14);
      have_format = true;
    } else if (std::memcmp(chunk, "data", 4) == 0) {
      clip.samples.resize(size / sizeof(i16));
      std::memcpy(clip.samples.data(), bytes.data() + body,
                  clip.samples.size() * sizeof(i16));
    }
    offset = body + size + (size & 1u); // chunks are word-aligned
  }

  if (!have_format || format != 1 || bits != 16 || clip.channels == 0 ||
      clip.channels > 2 || clip.samples.empty()) {
    KERROR("Sound '{}' must be 16-bit PCM, mono or stereo.", path);
    return std::nullopt;
  }
  return clip;
}

#if KPLATFORM_LINUX
// The background player -- see audio.h. Owns the clip cache and the queue;
// its one thread opens the ALSA device for each clip, plays it until it ends
// or a newer play() cuts it off, and closes it again, so a mismatched sample rate between two clips is
// never a problem and the device isn't held while nothing is playing.
class AudioPlayer {
public:
  ~AudioPlayer() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      queue_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  void play(std::string_view name) {
    std::lock_guard lock(mutex_);
    std::string key(name);
    auto it = clips_.find(key);
    if (it == clips_.end()) {
      if (failed_.contains(key)) {
        return; // already logged why -- see audio.h
      }
      std::optional<SoundClip> clip = load_wav("assets/sounds/" + key + ".wav");
      if (!clip) {
        failed_.insert(key);
        return;
      }
      it = clips_
               .emplace(key, std::make_shared<const SoundClip>(std::move(*clip)))
               .first;
    }
    // The newest cue wins: drop anything still waiting and tell the clip
    // that's playing to stop (play_blocking() watches generation_).
    queue_.clear();
    queue_.push_back(it->second);
    generation_.fetch_add(1, std::memory_order_relaxed);
    if (!thread_.joinable()) {
      thread_ = std::thread([this] { run(); });
    }
    wake_.notify_one();
  }

private:
  void run() {
    for (;;) {
      std::shared_ptr<const SoundClip> clip;
      u64 generation = 0;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) {
          return;
        }
        clip = queue_.front();
        queue_.pop_front();
        generation = generation_.load(std::memory_order_relaxed);
      }
      play_blocking(*clip, generation);
    }
  }

  // Plays `clip` unless a play() newer than `generation` arrives first, in
  // which case it stops mid-clip.
  void play_blocking(const SoundClip &clip, u64 generation) {
    snd_pcm_t *pcm = nullptr;
    int result = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (result < 0) {
      warn_device_once(result);
      return;
    }
    result = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                                SND_PCM_ACCESS_RW_INTERLEAVED, clip.channels,
                                clip.sample_rate, /*soft_resample=*/1,
                                /*latency_us=*/100000);
    if (result < 0) {
      warn_device_once(result);
      snd_pcm_close(pcm);
      return;
    }

    // Written in short slices rather than all at once so a newer play() is
    // noticed within a slice (plus the device's ~100ms latency) instead of
    // after the whole clip.
    const snd_pcm_uframes_t total = clip.samples.size() / clip.channels;
    const snd_pcm_uframes_t slice = std::max<snd_pcm_uframes_t>(
        clip.sample_rate / 50, 1); // 20ms
    snd_pcm_uframes_t written = 0;
    bool interrupted = false;
    while (written < total) {
      if (generation_.load(std::memory_order_relaxed) != generation) {
        interrupted = true;
        break;
      }
      snd_pcm_sframes_t frames =
          snd_pcm_writei(pcm, clip.samples.data() + written * clip.channels,
                         std::min(slice, total - written));
      if (frames < 0) {
        // An underrun or a suspend -- recover once and carry on; anything
        // it can't recover from ends this clip early.
        if (snd_pcm_recover(pcm, static_cast<int>(frames), /*silent=*/1) < 0) {
          break;
        }
        continue;
      }
      written += static_cast<snd_pcm_uframes_t>(frames);
    }
    if (interrupted) {
      snd_pcm_drop(pcm); // discard what's buffered -- cut off now
    } else {
      snd_pcm_drain(pcm);
    }
    snd_pcm_close(pcm);
  }

  void warn_device_once(int result) {
    if (!device_warned_) {
      device_warned_ = true;
      KWARN("No audio output ({}) -- sounds will be silent.",
            snd_strerror(result));
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::shared_ptr<const SoundClip>> queue_;
  std::unordered_map<std::string, std::shared_ptr<const SoundClip>> clips_;
  std::unordered_set<std::string> failed_;
  std::atomic<u64> generation_ = 0; // bumped by every play()
  bool stopping_ = false;
  bool device_warned_ = false; // only touched by the player thread
  std::thread thread_;
};

AudioPlayer &player() {
  static AudioPlayer instance;
  return instance;
}

// How long a track takes to fade out when it's replaced or stopped.
constexpr f32 kMusicFadeSeconds = 1.5f;

// The music player -- see audio.h. Its own thread and its own ALSA stream,
// separate from AudioPlayer's, so a cue never interrupts the music (the
// sound server mixes the two streams). The track is decoded by libmpg123 a
// slice at a time as it's written rather than up front, so a multi-minute
// track costs no load time and a few KB of memory.
class MusicPlayer {
public:
  ~MusicPlayer() {
    {
      std::lock_guard lock(mutex_);
      stopping_.store(true, std::memory_order_relaxed);
    }
    wake_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  void play(std::string_view name) {
    std::lock_guard lock(mutex_);
    if (requested_ == name) {
      return; // already playing (or fading to silence) -- see audio.h
    }
    requested_ = name;
    // stream() watches generation_ and fades out once it moves on.
    generation_.fetch_add(1, std::memory_order_relaxed);
    if (!thread_.joinable()) {
      thread_ = std::thread([this] { run(); });
    }
    wake_.notify_one();
  }

private:
  void run() {
    mpg123_init(); // a no-op since mpg123 1.27, required before it
    u64 consumed = 0;
    for (;;) {
      std::string track;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] {
          return stopping_.load(std::memory_order_relaxed) ||
                 generation_.load(std::memory_order_relaxed) != consumed;
        });
        if (stopping_.load(std::memory_order_relaxed)) {
          return;
        }
        track = requested_;
        consumed = generation_.load(std::memory_order_relaxed);
      }
      if (!track.empty() && !failed_.contains(track)) {
        stream(track, consumed);
      }
    }
  }

  // Loops `track` until a play()/stop() newer than `generation` arrives,
  // then fades it out and returns.
  void stream(const std::string &track, u64 generation) {
    const std::string path = "assets/sounds/" + track + ".mp3";
    int error = MPG123_OK;
    mpg123_handle *decoder = mpg123_new(nullptr, &error);
    if (!decoder) {
      KERROR("Music '{}': {}.", path, mpg123_plain_strerror(error));
      failed_.insert(track);
      return;
    }
    // Errors are reported through KERROR below, not libmpg123's stderr.
    mpg123_param(decoder, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
    // Always decode to 16-bit stereo, whatever the file is, so the ALSA
    // stream's format only depends on the sample rate.
    mpg123_format_none(decoder);
    const long *rates = nullptr;
    size_t rate_count = 0;
    mpg123_rates(&rates, &rate_count);
    for (size_t i = 0; i < rate_count; ++i) {
      mpg123_format(decoder, rates[i], MPG123_STEREO, MPG123_ENC_SIGNED_16);
    }
    long rate = 0;
    int channels = 0;
    int encoding = 0;
    if (mpg123_open(decoder, path.c_str()) != MPG123_OK ||
        mpg123_getformat(decoder, &rate, &channels, &encoding) != MPG123_OK) {
      KERROR("Music '{}' couldn't be opened: {}.", path,
             mpg123_strerror(decoder));
      failed_.insert(track);
      mpg123_delete(decoder);
      return;
    }

    snd_pcm_t *pcm = nullptr;
    int result = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (result >= 0) {
      result = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                                  SND_PCM_ACCESS_RW_INTERLEAVED, 2,
                                  static_cast<unsigned>(rate),
                                  /*soft_resample=*/1,
                                  /*latency_us=*/200000);
    }
    if (result < 0) {
      if (!device_warned_) {
        device_warned_ = true;
        KWARN("No audio output ({}) -- music will be silent.",
              snd_strerror(result));
      }
      if (pcm) {
        snd_pcm_close(pcm);
      }
      mpg123_close(decoder);
      mpg123_delete(decoder);
      return;
    }

    // 20ms slices, for the same reason as AudioPlayer::play_blocking().
    const size_t slice_frames = std::max<size_t>(static_cast<size_t>(rate) / 50, 1);
    std::vector<i16> slice(slice_frames * 2);
    const u64 fade_frames = std::max<u64>(
        static_cast<u64>(static_cast<f32>(rate) * kMusicFadeSeconds), 1);
    u64 faded = 0; // frames into the fade-out; 0 = not fading yet
    bool fading = false;
    bool rewound_empty = false; // guards against spinning on an empty file
    for (;;) {
      if (stopping_.load(std::memory_order_relaxed)) {
        snd_pcm_drop(pcm);
        break;
      }
      if (!fading && generation_.load(std::memory_order_relaxed) != generation) {
        fading = true;
      }
      if (fading && faded >= fade_frames) {
        snd_pcm_drain(pcm); // let the faded tail play out
        break;
      }

      size_t bytes = 0;
      const int read = mpg123_read(decoder, slice.data(),
                                   slice.size() * sizeof(i16), &bytes);
      const size_t frames = bytes / (2 * sizeof(i16));
      if (read == MPG123_DONE && frames == 0) {
        // End of the track -- loop back to the top.
        if (rewound_empty || mpg123_seek(decoder, 0, SEEK_SET) < 0) {
          snd_pcm_drain(pcm);
          break;
        }
        rewound_empty = true;
        continue;
      }
      if (read != MPG123_OK && read != MPG123_DONE &&
          read != MPG123_NEW_FORMAT) {
        KERROR("Music '{}' stopped: {}.", path, mpg123_strerror(decoder));
        failed_.insert(track);
        snd_pcm_drain(pcm);
        break;
      }
      if (frames == 0) {
        continue;
      }
      rewound_empty = false;

      if (fading) {
        for (size_t f = 0; f < frames; ++f, ++faded) {
          const f32 gain =
              faded >= fade_frames
                  ? 0.0f
                  : 1.0f - static_cast<f32>(faded) / static_cast<f32>(fade_frames);
          slice[f * 2] = static_cast<i16>(static_cast<f32>(slice[f * 2]) * gain);
          slice[f * 2 + 1] =
              static_cast<i16>(static_cast<f32>(slice[f * 2 + 1]) * gain);
        }
      }

      size_t written = 0;
      bool device_lost = false;
      while (written < frames) {
        snd_pcm_sframes_t n = snd_pcm_writei(
            pcm, slice.data() + written * 2, frames - written);
        if (n < 0) {
          // Same recover-once policy as AudioPlayer::play_blocking().
          if (snd_pcm_recover(pcm, static_cast<int>(n), /*silent=*/1) < 0) {
            device_lost = true;
            break;
          }
          continue;
        }
        written += static_cast<size_t>(n);
      }
      if (device_lost) {
        break;
      }
    }
    snd_pcm_close(pcm);
    mpg123_close(decoder);
    mpg123_delete(decoder);
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::string requested_;              // guarded by mutex_
  std::atomic<u64> generation_ = 0;    // bumped by every play() that changes track
  std::atomic<bool> stopping_ = false;
  // Only touched by the player thread.
  std::unordered_set<std::string> failed_;
  bool device_warned_ = false;
  std::thread thread_;
};

MusicPlayer &music() {
  static MusicPlayer instance;
  return instance;
}

#elif KPLATFORM_WINDOWS

// Rebuilds a canonical 44-byte-header WAV image from a parsed clip --
// PlaySound(SND_MEMORY) wants a whole RIFF file, and this way the source
// file's extra chunks (LIST, fact, ...) never reach it.
std::vector<char> make_wav_image(const SoundClip &clip) {
  const u32 data_bytes = static_cast<u32>(clip.samples.size() * sizeof(i16));
  const u16 block_align = static_cast<u16>(clip.channels * sizeof(i16));
  std::vector<char> image(44 + data_bytes);
  char *out = image.data();
  auto put = [&out](const void *src, size_t n) {
    std::memcpy(out, src, n);
    out += n;
  };
  auto put_u32 = [&put](u32 v) { put(&v, 4); }; // x86/ARM: little-endian
  auto put_u16 = [&put](u16 v) { put(&v, 2); };
  put("RIFF", 4);
  put_u32(36 + data_bytes);
  put("WAVE", 4);
  put("fmt ", 4);
  put_u32(16);
  put_u16(1); // PCM
  put_u16(static_cast<u16>(clip.channels));
  put_u32(clip.sample_rate);
  put_u32(clip.sample_rate * block_align);
  put_u16(block_align);
  put_u16(16);
  put("data", 4);
  put_u32(data_bytes);
  put(clip.samples.data(), data_bytes);
  return image;
}

// Windows player -- see audio.h. PlaySound(SND_ASYNC) already has exactly
// the semantics wanted: it returns immediately, plays on winmm's own
// thread, and a new call stops whatever the previous one is still playing.
// So unlike the ALSA player there's no thread or queue here, only the
// cache. SND_MEMORY reads the image in place while it plays, which is why
// images are kept for the life of the process.
class AudioPlayer {
public:
  void play(std::string_view name) {
    std::lock_guard lock(mutex_);
    std::string key(name);
    auto it = images_.find(key);
    if (it == images_.end()) {
      if (failed_.contains(key)) {
        return; // already logged why -- see audio.h
      }
      std::optional<SoundClip> clip = load_wav("assets/sounds/" + key + ".wav");
      if (!clip) {
        failed_.insert(key);
        return;
      }
      it = images_.emplace(key, make_wav_image(*clip)).first;
    }
    if (!PlaySoundA(it->second.data(), nullptr,
                    SND_MEMORY | SND_ASYNC | SND_NODEFAULT) &&
        !device_warned_) {
      device_warned_ = true;
      KWARN("No audio output -- sounds will be silent.");
    }
  }

private:
  std::mutex mutex_;
  std::unordered_map<std::string, std::vector<char>> images_;
  std::unordered_set<std::string> failed_;
  bool device_warned_ = false;
};

AudioPlayer &player() {
  // Deliberately never destroyed: winmm may still be reading a playing
  // image when the engine DLL unloads, and stopping it from a static
  // destructor would mean calling winmm under the loader lock.
  static AudioPlayer *instance = new AudioPlayer;
  return *instance;
}

// Windows music player -- see audio.h. MCI's "mpegvideo" device decodes
// MP3 itself and loops it with "play ... repeat", on its own thread and on
// a separate stream from PlaySound(), so like the cue player there's no
// thread here. MCI has no fade, so a change of track cuts straight over.
class MusicPlayer {
public:
  void play(std::string_view name) {
    std::lock_guard lock(mutex_);
    if (current_ == name) {
      return; // already playing -- see audio.h
    }
    close_current();
    current_ = name;
    if (current_.empty() || failed_.contains(current_)) {
      return;
    }
    // Absolute, since MCI resolves a relative path against its own idea of
    // the working directory rather than the process's.
    char full_path[MAX_PATH];
    const std::string path = "assets\\sounds\\" + current_ + ".mp3";
    if (GetFullPathNameA(path.c_str(), MAX_PATH, full_path, nullptr) == 0) {
      std::strncpy(full_path, path.c_str(), MAX_PATH - 1);
      full_path[MAX_PATH - 1] = '\0';
    }
    const std::string open =
        std::string("open \"") + full_path + "\" type mpegvideo alias " + kAlias;
    MCIERROR error = mciSendStringA(open.c_str(), nullptr, 0, nullptr);
    if (error == 0) {
      open_ = true;
      error = mciSendStringA((std::string("play ") + kAlias + " repeat").c_str(),
                             nullptr, 0, nullptr);
    }
    if (error != 0) {
      char message[256] = {};
      mciGetErrorStringA(error, message, sizeof(message));
      KERROR("Music '{}' couldn't be played: {}", path, message);
      failed_.insert(current_);
      close_current();
    }
  }

private:
  static constexpr const char *kAlias = "kengine_music";

  void close_current() {
    if (open_) {
      mciSendStringA((std::string("close ") + kAlias).c_str(), nullptr, 0,
                     nullptr);
      open_ = false;
    }
  }

  std::mutex mutex_;
  std::string current_;
  std::unordered_set<std::string> failed_;
  bool open_ = false;
};

MusicPlayer &music() {
  // Never destroyed, for the same loader-lock reason as player() above.
  static MusicPlayer *instance = new MusicPlayer;
  return *instance;
}

#endif

} // namespace

void audio_play_sound(std::string_view name) { player().play(name); }

void audio_play_music(std::string_view name) { music().play(name); }

void audio_stop_music() { music().play({}); }
