#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "src/wasm/wasm.pb.h"

namespace modlock::wasm {

// Futures numbers the calls a mod submitted without waiting and holds their
// replies until the host delivers them together. It is the table behind
// Submit and the Completed event; it runs no call itself, so the code that
// runs the calls, on whichever thread, completes each by number.
//
// Reserve, Complete, Take and Drop may be called from any thread. A call
// holds its place from Reserve until Take returns its reply, so a mod that
// never reads its replies cannot hold more than kMaxCalls. ServiceCalls is the
// other table of this shape, and differs in what it needs: its calls end by a
// provider's answer or a timeout, and a Futures call ends by the host's own
// run of it, never by time.
class Futures {
 public:
  // kMaxCalls is how many calls a mod holds that Take has not returned.
  static constexpr size_t kMaxCalls = 256;

  // Reserve numbers count calls and returns the first number; the rest follow
  // it. It refuses the whole batch when the mod would hold more than kMaxCalls.
  [[nodiscard]] std::expected<uint64_t, std::string> Reserve(size_t count);

  // Submit answers a Submit call: it numbers the calls the request holds and
  // calls start with each number and call in order, and returns the Reply that
  // carries the numbers. start must complete the call, now or later. A call
  // that is itself a Submit is completed with an error and not started. A
  // malformed request, or one that would hold too many calls, starts nothing
  // and returns the error.
  [[nodiscard]] Reply Submit(const Call& call,
                             const std::function<void(uint64_t, const Call&)>& start);

  // Complete records reply as call's answer, for the next Take.
  void Complete(uint64_t call, Reply reply);

  // Take returns the completed calls in number order, which is the order they
  // were submitted, and releases their places.
  [[nodiscard]] CompletedEvent Take();

  // Drop forgets every call, as when the mod stops. Numbers keep counting, so
  // a number is never reused.
  void Drop();

 private:
  std::mutex mu_;
  uint64_t next_ = 1;
  size_t held_ = 0;
  std::map<uint64_t, Reply> completed_;
};

}  // namespace modlock::wasm
