//go:build deps_only

#include "wasm/sandbox.h"

#include <algorithm>
#include <array>
#include <deque>
#include <string_view>
#include <utility>

namespace modlock::wasm {

namespace {

// kDecisions are the events whose reply the engine needs before it goes on,
// so a mod on its own thread takes them now or not at all: a hit's amount, a
// command's claim, a ping's block and a service call's answer.
constexpr std::array kDecisions = std::to_array<std::string_view>({
    "Damage",
    "Command",
    "Ping",
    "Serve",
});

bool Decides(std::string_view method) {
  return std::ranges::find(kDecisions, method) != kDecisions.end();
}

}  // namespace

struct Sandbox::Submitted {
  explicit Submitted(Instance::HostCall run) : run(std::move(run)) {}

  // Answer takes a Submit and queues its calls, and gives every other call to
  // the host.
  Reply Answer(const Call& call) {
    if (call.method() != "Submit") {
      return run(call);
    }
    return futures.Submit(
        call, [this](uint64_t id, const Call& submitted) { queue.emplace_back(id, submitted); });
  }

  Instance::HostCall run;
  Futures futures;
  std::deque<std::pair<uint64_t, Call>> queue;
};

std::expected<Sandbox, std::string> Sandbox::Load(Runtime& runtime, std::span<const uint8_t> module,
                                                  const Limits& limits,
                                                  Instance::HostCall host_call,
                                                  std::optional<ModThreadSettings> concurrent) {
  Sandbox sandbox;
  if (concurrent) {
    auto thread = ModThread::Load(runtime, module, limits, std::move(host_call), *concurrent);
    if (!thread) {
      return std::unexpected(std::move(thread.error()));
    }
    sandbox.thread_ = std::move(*thread);
  } else {
    auto submitted = std::make_shared<Submitted>(std::move(host_call));
    auto instance = Instance::Load(
        runtime, module, limits, [submitted](const Call& call) { return submitted->Answer(call); });
    if (!instance) {
      return std::unexpected(std::move(instance.error()));
    }
    sandbox.instance_ = std::move(*instance);
    sandbox.submitted_ = std::move(submitted);
  }
  return sandbox;
}

void Sandbox::Reset() {
  thread_.reset();
  instance_.reset();
  submitted_.reset();
  decided_.reset();
}

std::expected<Reply, std::string> Sandbox::Deliver(const Call& call) {
  if (!thread_) {
    return instance_->Deliver(call);
  }
  decided_.reset();
  if (call.method() == "Start") {
    return thread_->Begin(call);
  }
  if (!Decides(call.method())) {
    thread_->Post(call);
    return Reply();
  }
  decided_ = thread_->Decide(call);
  if (!decided_) {
    return std::unexpected(std::string(kBusy));
  }
  return decided_->reply;
}

std::optional<std::string> Sandbox::Failure() const {
  if (thread_) {
    return thread_->Failure();
  }
  return instance_ ? instance_->Failure() : std::nullopt;
}

std::optional<std::string> Sandbox::SlowReport() const {
  if (thread_) {
    if (decided_ && decided_->slow) {
      return decided_->report;
    }
    return std::nullopt;
  }
  if (instance_ && instance_->Slow()) {
    return instance_->Report();
  }
  return std::nullopt;
}

std::string Sandbox::Report() const {
  return instance_ ? instance_->Report() : std::string();
}

profile::SlowFrame Sandbox::Describe() const {
  return instance_ ? instance_->Describe() : profile::SlowFrame();
}

WindowStats Sandbox::Window(std::chrono::steady_clock::time_point deadline,
                            std::optional<Call> frame) {
  return thread_ ? thread_->Window(deadline, std::move(frame)) : WindowStats();
}

std::vector<Completion> Sandbox::Completed() {
  return thread_ ? thread_->Completed() : std::vector<Completion>();
}

std::optional<CompletedEvent> Sandbox::Flush() {
  // Hold the calls while they run, since one may stop the mod.
  const auto submitted = submitted_;
  if (!submitted || submitted->queue.empty()) {
    return std::nullopt;
  }
  for (auto& [id, call] : std::exchange(submitted->queue, {})) {
    submitted->futures.Complete(id, submitted->run(call));
  }
  CompletedEvent event = submitted->futures.Take();
  if (event.calls_size() == 0) {
    return std::nullopt;
  }
  return event;
}

}  // namespace modlock::wasm
