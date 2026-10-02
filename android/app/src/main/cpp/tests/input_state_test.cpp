#include "../input_state.h"

#include <condition_variable>
#include <exception>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
  using input::state::ordered_queue;
  using input::state::held_key_state;
  using packet = ordered_queue::packet;

  void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
  }

  int no_batch(packet &, packet &) { return 2; }

  // Simultaneously release competing threads without timing assumptions.
  class gate {
  public:
    void arrive() {
      std::unique_lock<std::mutex> lock(mutex_);
      if (++arrived_ == 2) {
        condition_.notify_all();
      } else {
        condition_.wait(lock, [this] { return arrived_ == 2; });
      }
    }
  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    unsigned arrived_ = 0;
  };

  void arrival_before_state() {
    ordered_queue queue;
    std::promise<void> arrival_enqueued;
    auto arrival_ready = arrival_enqueued.get_future();
    bool arrival_owner = false;
    bool state_owner = true;
    std::thread arrival([&] {
      arrival_owner = queue.enqueue(packet {1, 7});
      arrival_enqueued.set_value();
    });
    std::thread state([&] {
      arrival_ready.wait();
      state_owner = queue.enqueue(packet {2, 7});
    });
    arrival.join();
    state.join();
    require(arrival_owner && !state_owner, "arrival must acquire the only dispatcher");
    packet entry;
    require(queue.take(entry, no_batch) && entry == packet({1, 7}), "arrival must precede controller state");
    require(queue.take(entry, no_batch) && entry == packet({2, 7}), "controller state must follow its arrival");
    require(!queue.finishSlice(), "drained slice must relinquish ownership");
  }

  void arrival_during_dispatch() {
    ordered_queue queue;
    require(queue.enqueue(packet {1}), "first packet must schedule");
    packet entry;
    require(queue.take(entry, no_batch), "first packet must dispatch");
    bool new_owner = true;
    std::thread producer([&] { new_owner = queue.enqueue(packet {2}); });
    producer.join();
    require(!new_owner, "active dispatch must retain ownership after popping last packet");
    require(queue.finishSlice(), "arrival during dispatch must reschedule existing owner");
    require(queue.take(entry, no_batch) && entry == packet({2}), "rescheduled slice must dispatch arrival");
    require(!queue.finishSlice(), "empty queue must release dispatcher");
    require(queue.enqueue(packet {3}), "arrival after release must schedule a new dispatcher");
  }

  void enqueue_finish_race() {
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
      ordered_queue queue;
      require(queue.enqueue(packet {1}), "initial owner missing");
      packet entry;
      require(queue.take(entry, no_batch), "initial packet missing");
      gate start;
      bool reschedule = false;
      bool new_owner = false;
      std::thread dispatcher([&] {
        start.arrive();
        reschedule = queue.finishSlice();
      });
      std::thread producer([&] {
        start.arrive();
        new_owner = queue.enqueue(packet {2});
      });
      dispatcher.join();
      producer.join();
      require(reschedule != new_owner, "finish/enqueue race must create exactly one scheduler owner");
      require(queue.take(entry, no_batch) && entry == packet({2}), "racing arrival must not be lost");
      require(!queue.finishSlice(), "racing slice must drain cleanly");
    }
  }

  void coalescing_boundaries() {
    ordered_queue queue;
    queue.enqueue(packet {1});
    for (unsigned i = 0; i < 34; ++i) queue.enqueue(packet {1});
    unsigned scanned = 0;
    packet entry;
    require(queue.take(entry, [&](packet &dest, packet &src) {
      ++scanned;
      dest[0] += src[0];
      return 0;
    }), "coalesced packet missing");
    require(scanned == 32 && entry == packet({33}), "coalescing must scan at most 32 candidates");
    require(queue.take(entry, no_batch) && entry == packet({1}), "first candidate beyond scan bound must remain");
    require(queue.take(entry, no_batch) && entry == packet({1}), "second candidate beyond scan bound must remain");
    require(!queue.finishSlice(), "bounded scan left unexpected entries");

    queue.enqueue(packet {1});
    queue.enqueue(packet {2});
    queue.enqueue(packet {1});
    queue.enqueue(packet {3});
    queue.enqueue(packet {1});
    require(queue.take(entry, [](packet &dest, packet &src) {
      if (src[0] == 3) return 2;
      if (src[0] == 2) return 1;
      dest[0] += src[0];
      return 0;
    }) && entry == packet({2}), "skip must permit later merging before termination");
    require(queue.take(entry, no_batch) && entry == packet({2}), "skipped packet must remain in order");
    require(queue.take(entry, no_batch) && entry == packet({3}), "termination boundary must remain");
    require(queue.take(entry, no_batch) && entry == packet({1}), "packet after termination must not merge");
    require(!queue.finishSlice(), "coalescing boundary queue must drain");
  }

  void reset_invalidates_pending_work() {
    ordered_queue queue;
    require(queue.enqueue(packet {1}), "initial reset fixture owner missing");
    queue.enqueue(packet {2});
    const auto delayed_generation = queue.generation();
    unsigned delayed_releases = 0;
    auto delayed_release = [&] {
      if (queue.valid(delayed_generation)) ++delayed_releases;
    };
    require(queue.valid(delayed_generation), "captured generation must initially be valid");
    queue.reset();
    delayed_release();
    require(delayed_releases == 0, "reset must invalidate delayed device operations");
    require(queue.generation() == delayed_generation + 1, "reset must advance generation once");
    packet entry;
    require(!queue.take(entry, no_batch), "reset must discard queued packets");
    require(!queue.enqueue(packet {3}), "reset must retain ownership of pending scheduler");
    require(queue.finishSlice(), "pending owner must pick up post-reset arrival");
    require(queue.take(entry, no_batch) && entry == packet({3}), "only post-reset input may dispatch");
    require(!queue.finishSlice(), "post-reset dispatcher must release when drained");
    require(queue.enqueue(packet {4}), "new scheduler must be possible after reset owner finishes");
    queue.reset();
    require(!queue.finishSlice(), "reset without further arrivals must let pending owner finish");
    require(queue.enqueue(packet {5}), "reset must not strand next input");
  }

  void extended_key_identity() {
    held_key_state key;
    require(!key.update(false, false), "orphan key-up must not create a transition");
    require(key.update(true, true), "extended key-down must transition");
    require(key.pressed && key.extended, "key repeat must retain extended identity");
    require(!key.update(true, false) && key.extended, "duplicate down without flags must not change held identity");
    require(key.update(false, false), "key-up without extended flags must release held key");
    require(!key.pressed && key.extended, "release must use key-down extended identity");
    require(!key.update(false, false), "duplicate release must not transition");
    require(key.update(true, false) && !key.extended, "next nonextended press must replace prior identity");
    require(key.update(false, true) && !key.extended, "key-up flags must not override nonextended press identity");
    require(key.update(true, true), "extended key must be held before reset");
    key = {};
    require(!key.pressed && !key.extended, "reset must discard held key state");
    require(key.update(true, false) && !key.extended, "post-reset press must use its own flags");
  }
}

int main() {
  try {
    arrival_before_state();
    arrival_during_dispatch();
    enqueue_finish_race();
    coalescing_boundaries();
    reset_invalidates_pending_work();
    extended_key_identity();
    std::cout << "input state behavior tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
