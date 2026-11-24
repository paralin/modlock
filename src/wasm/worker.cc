//go:build deps_only

#include "wasm/worker.h"

#include <chrono>
#include <format>
#include <iostream>
#include <utility>

#include "src/wasm/host_service.gen.h"
#include "wasm/instance.h"

namespace modlock::wasm {
namespace {

using Clock = std::chrono::steady_clock;

// kWorkerMemory caps a worker's linear memory; a table of tens of megabytes
// and its decoded copy fit with room to spare.
constexpr int64_t kWorkerMemory = int64_t{512} << 20;

// kWorkerEvent bounds one request. A worker exists to run long work, so the
// limit stops only a runaway loop.
constexpr std::chrono::milliseconds kWorkerEvent{120'000};

// Milliseconds returns the time since began in milliseconds.
double Milliseconds(Clock::time_point began) {
  return std::chrono::duration<double, std::milli>(Clock::now() - began).count();
}

// WorkerHost is everything a worker's script may call. HostService refuses
// each call it does not override.
class WorkerHost : public HostService {
 public:
  WorkerHost(ModFiles files, std::function<void(const std::string&)> log)
      : files_(std::move(files)), log_(std::move(log)) {}

  std::expected<void, std::string> Log(const LogRequest& request) override {
    log_(request.message());
    return {};
  }

  std::expected<ReadFileResponse, std::string> ReadFile(const ReadFileRequest& request) override {
    auto text = files_.Read(request.path());
    if (!text) {
      return std::unexpected(text.error());
    }
    ReadFileResponse response;
    if (*text) {
      response.set_text(std::move(**text));
    }
    return response;
  }

  std::expected<ReadFileBytesResponse, std::string> ReadFileBytes(
      const ReadFileRequest& request) override {
    auto data = files_.Read(request.path());
    if (!data) {
      return std::unexpected(data.error());
    }
    ReadFileBytesResponse response;
    if (*data) {
      response.set_data(std::move(**data));
    }
    return response;
  }

 private:
  ModFiles files_;
  std::function<void(const std::string&)> log_;
};

}  // namespace

Worker::Worker(Runtime& runtime, std::span<const uint8_t> module, std::string source,
               ModFiles files, std::string name)
    : runtime_(runtime),
      module_(module.begin(), module.end()),
      source_(std::move(source)),
      files_(std::move(files)),
      name_(std::move(name)),
      thread_([this](std::stop_token stop) { Run(stop); }) {}

Worker::~Worker() {
  thread_.request_stop();
  wake_.notify_all();
  thread_.join();
  for (auto& request : queue_) {
    request.reply(std::unexpected("the worker stopped"));
  }
}

void Worker::Post(std::string payload, WasmReply reply) {
  std::unique_lock lock(mu_);
  if (failure_) {
    auto failure = *failure_;
    lock.unlock();
    reply(std::unexpected(std::move(failure)));
    return;
  }
  queue_.push_back({.payload = std::move(payload), .reply = std::move(reply)});
  lock.unlock();
  wake_.notify_one();
}

void Worker::Log(const std::string& text) const {
  // One write, so lines from the engine thread and this one stay whole.
  std::cerr << std::format("[{}] {}\n", name_, text);
}

void Worker::Fail(const std::string& failure) {
  // The store is gone for good: refuse what waits and everything posted later.
  Log(failure);
  std::deque<Request> waiting;
  {
    std::lock_guard lock(mu_);
    failure_ = failure;
    waiting.swap(queue_);
  }
  for (auto& request : waiting) {
    request.reply(std::unexpected(failure));
  }
}

void Worker::Run(std::stop_token stop) {
  // Load the store and start the script, timing each step.
  const auto began = Clock::now();
  WorkerHost host(files_, [this](const std::string& text) { Log(text); });
  Limits limits;
  limits.memory_bytes = kWorkerMemory;
  limits.event_budget = kWorkerEvent;
  limits.slow_event = kWorkerEvent;
  auto instance = Instance::Load(runtime_, module_, limits,
                                 [&host](const Call& call) { return host.Dispatch(call); });
  std::optional<std::string> failure;
  const double loaded = Milliseconds(began);
  if (instance) {
    StartEvent event;
    event.set_source(source_);
    Call start;
    start.set_method("Start");
    start.set_request(event.SerializeAsString());
    if (auto started = (*instance)->Deliver(start); !started) {
      failure = "the worker's script did not start: " + started.error();
    } else if (!started->error().empty()) {
      failure = "the worker's script did not start: " + started->error();
    }
  } else {
    failure = "the worker did not load: " + instance.error();
  }
  if (failure) {
    Fail(*failure);
    return;
  }
  Log(std::format("worker ready in {:.1f} ms ({:.1f} ms to load the store), memory {} KiB", Milliseconds(began),
                  loaded, (*instance)->MemoryBytes() >> 10));

  // Answer requests one at a time until the mod stops.
  uint64_t served = 0;
  for (;;) {
    Request request;
    {
      std::unique_lock lock(mu_);
      if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); })) {
        return;
      }
      request = std::move(queue_.front());
      queue_.pop_front();
    }
    const auto requested = Clock::now();
    Call call;
    call.set_method("Request");
    const auto in = request.payload.size();
    call.set_request(std::move(request.payload));
    auto answered = (*instance)->Deliver(call);
    const double took = Milliseconds(requested);
    if (!answered) {
      Log(std::format("request {} failed after {:.1f} ms: {}", ++served, took, answered.error()));
      request.reply(std::unexpected(answered.error()));
      Fail(answered.error());
      return;
    }
    Log(std::format("request {}: {:.1f} ms, {} bytes in, {} bytes out, memory {} KiB", ++served,
                    took, in, answered->response().size(), (*instance)->MemoryBytes() >> 10));
    if (!answered->error().empty()) {
      request.reply(std::unexpected(answered->error()));
    } else {
      request.reply(std::move(*answered->mutable_response()));
    }
  }
}

}  // namespace modlock::wasm
