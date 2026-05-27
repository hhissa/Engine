#pragma once

#include "../defines.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

struct EventContext {
  union {
    i16 i64[2];
    u64 u64[2];
    f64 f64[2];
    i32 i32[4];
    u32 u32[4];
    f32 f32[4];
    i16 i16[8];
    u16 u16[8];
    i8 i8[16];
    u8 u8[16];
    char c[16];
  } data;
};

enum class EventCode : std::uint16_t {
  // Shuts the application down on the next frame.
  EVENT_CODE_APPLICATION_QUIT = 0x01,

  // Keyboard key pressed.
  /* Context usage:
   * u16 key_code = data.data.u16[0];
   */
  EVENT_CODE_KEY_PRESSED = 0x02,

  // Keyboard key released.
  /* Context usage:
   * u16 key_code = data.data.u16[0];
   */
  EVENT_CODE_KEY_RELEASED = 0x03,

  // Mouse button pressed.
  /* Context usage:
   * u16 button = data.data.u16[0];
   */
  EVENT_CODE_BUTTON_PRESSED = 0x04,

  // Mouse button released.
  /* Context usage:
   * u16 button = data.data.u16[0];
   */
  EVENT_CODE_BUTTON_RELEASED = 0x05,

  // Mouse moved.
  /* Context usage:
   * u16 x = data.data.u16[0];
   * u16 y = data.data.u16[1];
   */
  EVENT_CODE_MOUSE_MOVED = 0x06,

  // Mouse moved.
  /* Context usage:
   * u8 z_delta = data.data.u8[0];
   */
  EVENT_CODE_MOUSE_WHEEL = 0x07,

  // Resized/resolution changed from the OS.
  /* Context usage:
   * u16 width = data.data.u16[0];
   * u16 height = data.data.u16[1];
   */
  EVENT_CODE_RESIZED = 0x08,

  MAX_EVENT_CODE = 0xFF
};

class EventSystem;

// RAII handle: unregisters automatically when destroyed.
// Must not outlive the EventSystem that issued it.
class [[nodiscard]] EventSubscription {
public:
  EventSubscription() = default;
  ~EventSubscription();

  EventSubscription(EventSubscription &&other) noexcept;
  EventSubscription &operator=(EventSubscription &&other) noexcept;

  EventSubscription(const EventSubscription &) = delete;
  EventSubscription &operator=(const EventSubscription &) = delete;

  void release(); // unregister early
  explicit operator bool() const { return id_ != 0; }

private:
  friend class EventSystem;
  EventSubscription(EventSystem *sys, std::uint64_t id) : sys_(sys), id_(id) {}
  EventSystem *sys_ = nullptr;
  std::uint64_t id_ = 0;
};

class KAPI EventSystem {
public:
  using Callback =
      std::function<bool(EventCode, void *sender, const EventContext &)>;

  EventSystem() = default;
  ~EventSystem() = default;

  // Non-copyable, non-movable: outstanding EventSubscriptions hold a
  // pointer back to this instance.
  EventSystem(const EventSystem &) = delete;
  EventSystem &operator=(const EventSystem &) = delete;
  EventSystem(EventSystem &&) = delete;
  EventSystem &operator=(EventSystem &&) = delete;

  bool initialize();
  void shutdown();

  // Returns an empty subscription on failure.
  EventSubscription subscribe(EventCode code, Callback cb);

  bool fire(EventCode code, void *sender, const EventContext &ctx);

private:
  friend class EventSubscription;
  void unsubscribe(std::uint64_t id);

  struct RegisteredEvent {
    std::uint64_t id;
    Callback callback;
  };

  bool initialized_ = false;
  std::uint64_t next_id_ = 1; // 0 reserved for "empty subscription"
  std::unordered_map<EventCode, std::vector<RegisteredEvent>> registered_;
};
