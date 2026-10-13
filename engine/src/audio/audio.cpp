#include "audio.h"

#include "../core/logger.h"

#include <alsa/asoundlib.h>

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

} // namespace

void audio_play_sound(std::string_view name) { player().play(name); }
