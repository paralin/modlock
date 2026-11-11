#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "modlock/wasm_host.h"
#include "src/wasm/wasm.pb.h"

namespace modlock::wasm {

// ServiceCalls holds one mod's started service calls until each ends: with
// the provider's answer or refusal, a timeout, or no provider. The host's
// frame thread starts and takes calls; a provider may answer from any
// thread, once. Dropping the calls ends the instance's waits without an
// event, and an answer to a dropped call does nothing.
class ServiceCalls {
 public:
  // kMaxTimeout bounds how long a call waits, and kMaxCalls how many calls a
  // mod holds between frames, ended or waiting.
  static constexpr std::chrono::milliseconds kMaxTimeout{30'000};
  static constexpr size_t kMaxCalls = 256;

  // Started numbers one call and gives the provider the reply that ends it.
  struct Started {
    uint64_t call = 0;
    WasmReply reply;
  };

  ServiceCalls();

  // Start numbers a call that waits timeout, at most kMaxTimeout, or zero
  // for kMaxTimeout. It refuses a call past kMaxCalls.
  [[nodiscard]] std::expected<Started, std::string> Start(std::chrono::milliseconds timeout);

  // Fail ends call at once with failure and error, as when nobody provides
  // its service.
  void Fail(uint64_t call, ServiceFailure failure, std::string error);

  // Take returns the calls that ended by now, oldest first, and forgets
  // them.
  [[nodiscard]] std::vector<ServiceRepliedEvent> Take(std::chrono::steady_clock::time_point now);

  // Drop forgets every call, so the answers still to come do nothing.
  void Drop();

 private:
  // Call is one started call: its deadline and, once it ended, its event.
  struct Call {
    std::chrono::steady_clock::time_point deadline;
    std::optional<ServiceRepliedEvent> ended;
  };

  // Shared is what the replies reach: the calls by number, under a lock,
  // since a provider may answer from another thread.
  struct Shared {
    mutable std::mutex lock;
    std::map<uint64_t, Call> calls;
  };

  // End records event as call's ending, unless it already ended or was
  // forgotten.
  static void End(Shared& shared, uint64_t call, ServiceRepliedEvent event);

  std::shared_ptr<Shared> shared_;
  uint64_t next_ = 1;
};

}  // namespace modlock::wasm
