#include "event.h"

#include <algorithm>
#include <utility>

// ---------------- EventSubscription ----------------

EventSubscription::~EventSubscription() { release(); }

EventSubscription::EventSubscription(EventSubscription &&other) noexcept
    : sys_(other.sys_), id_(other.id_) {
  other.sys_ = nullptr;
  other.id_ = 0;
}

EventSubscription &
EventSubscription::operator=(EventSubscription &&other) noexcept {
  if (this != &other) {
    release();
    sys_ = other.sys_;
    id_ = other.id_;
    other.sys_ = nullptr;
    other.id_ = 0;
  }
  return *this;
}

void EventSubscription::release() {
  if (sys_ && id_ != 0) {
    sys_->unsubscribe(id_);
  }
  sys_ = nullptr;
  id_ = 0;
}

// ---------------- EventSystem ----------------

bool EventSystem::initialize() {
  if (initialized_) {
    return false;
  }
  registered_.clear();
  next_id_ = 1;
  initialized_ = true;
  return true;
}

void EventSystem::shutdown() {
  registered_.clear();
  initialized_ = false;
}

EventSubscription EventSystem::subscribe(EventCode code, Callback cb) {
  if (!initialized_ || !cb) {
    return EventSubscription{};
  }

  const std::uint64_t id = next_id_++;
  registered_[code].push_back(RegisteredEvent{id, std::move(cb)});
  return EventSubscription{this, id};
}

bool EventSystem::fire(EventCode code, void *sender, const EventContext &ctx) {
  if (!initialized_) {
    return false;
  }

  auto it = registered_.find(code);
  if (it == registered_.end() || it->second.empty()) {
    return false;
  }

  // Copy so callbacks can safely (un)subscribe during dispatch.
  auto snapshot = it->second;
  for (auto &entry : snapshot) {
    if (entry.callback && entry.callback(code, sender, ctx)) {
      return true; // handled — stop propagating
    }
  }
  return false;
}

void EventSystem::unsubscribe(std::uint64_t id) {
  if (!initialized_ || id == 0) {
    return;
  }

  for (auto &[code, vec] : registered_) {
    auto it =
        std::find_if(vec.begin(), vec.end(),
                     [id](const RegisteredEvent &e) { return e.id == id; });
    if (it != vec.end()) {
      vec.erase(it);
      return;
    }
  }
}
