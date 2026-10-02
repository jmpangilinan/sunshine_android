#include "../thread_safe.h"
#include <cstdlib>
#include <memory>

static void require(bool condition) {
    if (!condition) std::abort();
}

int main() {
    safe::queue_t<std::unique_ptr<int>> frames(2);
    require(frames.try_raise(std::make_unique<int>(101)));
    require(frames.try_raise(std::make_unique<int>(102)));
    auto rejected = std::make_unique<int>(103);
    require(!frames.try_raise(std::move(rejected)));
    require(rejected && *rejected == 103);
    auto first = frames.pop();
    auto second = frames.pop();
    require(first && *first == 101);
    require(second && *second == 102);
    require(frames.try_raise(std::move(rejected)));
    auto third = frames.pop();
    require(third && *third == 103);

    // A stopped queue rejects late producers and cannot deliver a late press/frame.
    require(frames.try_raise(std::make_unique<int>(104)));
    frames.stop();
    auto late = std::make_unique<int>(105);
    require(!frames.try_raise(std::move(late)));
    require(late && *late == 105);
    require(!frames.pop());
}
