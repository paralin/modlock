#pragma once

#include <algorithm>
#include <functional>
#include <type_traits>

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

  Result Dispatch(Args... args) {
    std::erase_if(callbacks_, [](const auto& callback) { return callback.expired(); });
    const auto current = callbacks_;
    for (const auto& weak : current) {
      if (auto callback = weak.lock()) {
        if constexpr (std::is_same_v<Result, bool>) {
          if ((*callback)(args...)) {
            return true;
          }
        } else {
          (*callback)(args...);
        }
      }
    }
    if constexpr (std::is_same_v<Result, bool>) {
      return false;
    }
  }

 private:
  std::vector<std::weak_ptr<Callback>> callbacks_;
};

}  // namespace modlock
