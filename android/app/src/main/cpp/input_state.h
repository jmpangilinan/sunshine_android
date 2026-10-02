#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <utility>
#include <vector>

namespace input::state {

  // State transitions are serialized by the caller's input dispatch lock.
  struct held_key_state {
    bool pressed = false;
    bool extended = false;

    // Key-up packets need not repeat the flags from key-down.
    bool update(bool down, bool packet_extended) {
      if (pressed == down) return false;
      if (down) extended = packet_extended;
      pressed = down;
      return true;
    }
  };

  // One scheduler owner drains this queue in bounded slices. Enqueue callers
  // schedule outside this mutex, and only when enqueue returns true.
  class ordered_queue {
  public:
    using packet = std::vector<std::uint8_t>;
    static constexpr unsigned scan_limit = 32;

    bool enqueue(packet &&entry) {
      std::lock_guard<std::mutex> lock(mutex_);
      entries_.push_back(std::move(entry));
      if (scheduled_) return false;
      scheduled_ = true;
      return true;
    }

    // Coalesce returns 0 to consume the candidate, 1 to skip it, or 2 to
    // stop scanning. It runs under the queue mutex and must not reenter it.
    // Empty does not release ownership: always finish the scheduler slice.
    template<class Coalesce>
    bool take(packet &entry, Coalesce &&coalesce) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (entries_.empty()) return false;
      entry = std::move(entries_.front());
      entries_.pop_front();
      auto candidate = entries_.begin();
      for (unsigned scanned = 0; candidate != entries_.end() && scanned < scan_limit; ++scanned) {
        const auto result = coalesce(entry, *candidate);
        if (result == 2) break;
        if (result == 0) candidate = entries_.erase(candidate);
        else ++candidate;
      }
      return true;
    }

    // True transfers the existing ownership to the next scheduled slice.
    // False releases it atomically with the empty check, so an arrival either
    // belongs to this owner or acquires a new one, never neither or both.
    bool finishSlice() {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!entries_.empty()) return true;
      scheduled_ = false;
      return false;
    }

    // Caller holds its dispatch lock while resetting device state. A pending
    // scheduler still owns its task and will drain arrivals after the reset.
    void reset() {
      std::lock_guard<std::mutex> lock(mutex_);
      entries_.clear();
      ++generation_;
    }

    std::uint64_t generation() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return generation_;
    }

    // Caller must hold its dispatch lock from this check through the device
    // operation, preventing reset between validation and the operation.
    bool valid(std::uint64_t generation) const {
      std::lock_guard<std::mutex> lock(mutex_);
      return generation == generation_;
    }

  private:
    mutable std::mutex mutex_;
    std::list<packet> entries_;
    bool scheduled_ = false;
    std::uint64_t generation_ = 0;
  };

}  // namespace input::state
