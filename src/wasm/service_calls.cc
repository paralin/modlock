//go:build deps_only

#include "wasm/service_calls.h"

#include <utility>

namespace modlock::wasm {

ServiceCalls::ServiceCalls() : shared_(std::make_shared<Shared>()) {}

std::expected<ServiceCalls::Started, std::string> ServiceCalls::Start(
    std::chrono::milliseconds timeout) {
  // Bound the wait, and the calls a mod holds between frames.
  if (timeout <= std::chrono::milliseconds::zero() || timeout > kMaxTimeout) {
    timeout = kMaxTimeout;
  }
  const auto call = next_;
  {
    std::lock_guard held(shared_->lock);
    if (shared_->calls.size() >= kMaxCalls) {
      return std::unexpected("the mod already holds " + std::to_string(kMaxCalls) +
                             " service calls");
    }
    shared_->calls.emplace(call, Call{.deadline = std::chrono::steady_clock::now() + timeout});
  }
  ++next_;

  // The reply reaches the calls only while this instance holds them, and
  // ends the call once.
  std::weak_ptr<Shared> reached = shared_;
  WasmReply reply = [reached, call](std::expected<std::string, std::string> answer) {
    auto shared = reached.lock();
    if (!shared) {
      return;
    }
    ServiceRepliedEvent event;
    event.set_call(call);
    if (answer) {
      event.set_payload(std::move(*answer));
    } else {
      event.set_failure(SERVICE_FAILURE_REFUSED);
      event.set_error(std::move(answer.error()));
    }
    End(*shared, call, std::move(event));
  };
  return Started{.call = call, .reply = std::move(reply)};
}

void ServiceCalls::Fail(uint64_t call, ServiceFailure failure, std::string error) {
  ServiceRepliedEvent event;
  event.set_call(call);
  event.set_failure(failure);
  event.set_error(std::move(error));
  End(*shared_, call, std::move(event));
}

std::vector<ServiceRepliedEvent> ServiceCalls::Take(std::chrono::steady_clock::time_point now) {
  // Collect the ended calls and those past their deadline, in call order.
  std::vector<ServiceRepliedEvent> ended;
  std::lock_guard held(shared_->lock);
  for (auto it = shared_->calls.begin(); it != shared_->calls.end();) {
    if (it->second.ended) {
      ended.push_back(std::move(*it->second.ended));
    } else if (now >= it->second.deadline) {
      ServiceRepliedEvent event;
      event.set_call(it->first);
      event.set_failure(SERVICE_FAILURE_TIMEOUT);
      event.set_error("the service did not answer in time");
      ended.push_back(std::move(event));
    } else {
      ++it;
      continue;
    }
    it = shared_->calls.erase(it);
  }
  return ended;
}

void ServiceCalls::Drop() {
  // A fresh table leaves the replies still out holding nothing they reach.
  shared_ = std::make_shared<Shared>();
}

void ServiceCalls::End(Shared& shared, uint64_t call, ServiceRepliedEvent event) {
  std::lock_guard held(shared.lock);
  auto found = shared.calls.find(call);
  if (found == shared.calls.end() || found->second.ended) {
    return;
  }
  found->second.ended = std::move(event);
}

}  // namespace modlock::wasm
