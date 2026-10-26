#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

#include "modlock/subscription.h"

namespace modlock {

// Callbacks borrows registrations from their subscriptions. Dispatch shares the
// active registration until its call returns; expired registrations own no code
// or captures, so unloading a stopped plugin leaves no deferred DLL destructor.
template <typename Signature>
class Callbacks;

template <typename Result, typename... Args>
class Callbacks<Result(Args...)> {
 public:
  using Callback = std::function<Result(Args...)>;

  Subscription Add(Callback callback) {
    if (!callback) {
      return {};
    }
    auto registration = std::make_shared<Callback>(std::move(callback));
    callbacks_.push_back(registration);
    return Subscription(std::move(registration));
  }

  // Dispatch calls the registrations present when it starts, in order; a
  // bool callback that returns true claims the dispatch and stops it. A
  // callback may add or remove registrations: one it adds waits for the next
  // dispatch, and one it removes is skipped. Dispatch copies nothing, so a
  // frame's dispatch allocates nothing.
  Result Dispatch(Args... args) {
    // Prune the expired registrations unless an outer dispatch is iterating.
    if (depth_ == 0) {
      std::erase_if(callbacks_, [](const auto& callback) { return callback.expired(); });
    }

    // Call each registration by index, since one added meanwhile may move the
    // vector; it lands past count.
    const Depth depth(depth_);
    const size_t count = callbacks_.size();
    for (size_t index = 0; index < count; ++index) {
      const auto callback = callbacks_[index].lock();
      if (!callback) {
        continue;
      }
      if constexpr (std::is_same_v<Result, bool>) {
        if ((*callback)(args...)) {
          return true;
        }
      } else {
        (*callback)(args...);
      }
    }
    if constexpr (std::is_same_v<Result, bool>) {
      return false;
    }
  }

 private:
  // Depth counts the dispatches running for as long as it lives.
  class Depth {
   public:
    explicit Depth(int& depth) : depth_(depth) { ++depth_; }

    ~Depth() { --depth_; }

    Depth(const Depth&) = delete;
    Depth& operator=(const Depth&) = delete;

   private:
    int& depth_;
  };

  std::vector<std::weak_ptr<Callback>> callbacks_;
  // depth_ is the number of dispatches running, which a nested dispatch
  // must not prune under.
  int depth_ = 0;
};

}  // namespace modlock
