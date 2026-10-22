#include "callbacks.h"

#include <gtest/gtest.h>

namespace modlock {
namespace {

TEST(Callbacks, ClaimsInRegistrationOrderAndReleasesCaptures) {
  // Register three callbacks; the first holds a capture.
  Callbacks<bool()> callbacks;
  std::vector<int> calls;
  auto lifetime = std::make_shared<int>(1);
  auto first = callbacks.Add([&, lifetime] {
    calls.push_back(*lifetime);
    return false;
  });
  auto second = callbacks.Add([&] {
    calls.push_back(2);
    return true;
  });
  auto third = callbacks.Add([&] {
    calls.push_back(3);
    return false;
  });

  // Dispatch stops at the first claim.
  std::weak_ptr<int> observed = lifetime;
  lifetime.reset();
  EXPECT_TRUE(callbacks.Dispatch());
  EXPECT_EQ(calls, (std::vector<int>{1, 2}));

  // Resetting a subscription releases its capture and removes it.
  first.Reset();
  EXPECT_TRUE(observed.expired());
  second.Reset();
  calls.clear();
  EXPECT_FALSE(callbacks.Dispatch());
  EXPECT_EQ(calls, (std::vector<int>{3}));
}

TEST(Callbacks, DispatchAllowsRemovalAndDefersNewRegistrations) {
  // The first callback removes the second and adds a third.
  Callbacks<void()> callbacks;
  std::vector<int> calls;
  Subscription second;
  Subscription added;
  auto first = callbacks.Add([&] {
    calls.push_back(1);
    second.Reset();
    added = callbacks.Add([&] { calls.push_back(3); });
  });
  second = callbacks.Add([&] { calls.push_back(2); });
  auto empty = callbacks.Add({});

  // Dispatch skips the removed one and defers the new one.
  callbacks.Dispatch();
  EXPECT_EQ(calls, (std::vector<int>{1}));
  first.Reset();
  callbacks.Dispatch();
  EXPECT_EQ(calls, (std::vector<int>{1, 3}));
}

}  // namespace
}  // namespace modlock
