//go:build deps_only

#include "wasm/futures.h"

#include <utility>

namespace modlock::wasm {

std::expected<uint64_t, std::string> Futures::Reserve(size_t count) {
  const std::lock_guard lock(mu_);
  if (count > kMaxCalls || held_ + count > kMaxCalls) {
    return std::unexpected("the mod already holds " + std::to_string(held_) +
                           " submitted calls, and may hold " + std::to_string(kMaxCalls));
  }
  const uint64_t first = next_;
  next_ += count;
  held_ += count;
  return first;
}

Reply Futures::Submit(const Call& call, const std::function<void(uint64_t, const Call&)>& start) {
  Reply reply;
  SubmitRequest request;
  if (!request.ParseFromString(call.request())) {
    reply.set_error("the Submit request is malformed");
    return reply;
  }
  const auto first = Reserve(static_cast<size_t>(request.calls_size()));
  if (!first) {
    reply.set_error(first.error());
    return reply;
  }

  // Number the calls and start each in the order submitted.
  SubmitResponse response;
  uint64_t id = *first;
  for (const Call& submitted : request.calls()) {
    response.add_calls(id);
    if (submitted.method() == "Submit") {
      Reply refused;
      refused.set_error("a submitted call cannot submit");
      Complete(id, std::move(refused));
    } else {
      start(id, submitted);
    }
    ++id;
  }
  reply.set_response(response.SerializeAsString());
  return reply;
}

void Futures::Complete(uint64_t call, Reply reply) {
  const std::lock_guard lock(mu_);
  completed_.insert_or_assign(call, std::move(reply));
}

CompletedEvent Futures::Take() {
  CompletedEvent event;
  const std::lock_guard lock(mu_);
  for (auto& [call, reply] : completed_) {
    CompletedCall* done = event.add_calls();
    done->set_call(call);
    *done->mutable_reply() = std::move(reply);
  }
  held_ -= completed_.size();
  completed_.clear();
  return event;
}

void Futures::Drop() {
  const std::lock_guard lock(mu_);
  completed_.clear();
  held_ = 0;
}

}  // namespace modlock::wasm
