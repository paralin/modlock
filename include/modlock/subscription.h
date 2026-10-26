#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "modlock/export.h"

namespace modlock {

// Subscription owns callback registrations. Releasing it removes its callbacks
// before captured plugin state is destroyed. Subscribe and release occur on the
// engine thread, or before engine startup and after engine shutdown.
class MODLOCK_API Subscription {
 public:
  Subscription() = default;

  explicit Subscription(std::shared_ptr<void> registration) {
    registrations_.push_back(std::move(registration));
  }

  Subscription(Subscription&&) noexcept = default;
  Subscription& operator=(Subscription&&) noexcept = default;
  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;

  // Add joins another registration to this lifetime.
  void Add(Subscription other) {
    for (auto& registration : other.registrations_) {
      registrations_.push_back(std::move(registration));
    }
  }

  // Active reports whether the subscription owns any registration.
  bool Active() const { return !registrations_.empty(); }

  // Reset removes every owned callback; an in-flight callback may finish.
  void Reset() { registrations_.clear(); }

 private:
  // Dispatch temporarily shares a registration while invoking its callback.
  std::vector<std::shared_ptr<void>> registrations_;
};

}  // namespace modlock
