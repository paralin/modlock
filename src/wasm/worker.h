#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "modlock/wasm_host.h"
#include "wasm/files.h"
#include "wasm/runtime.h"

namespace modlock::wasm {

// kWorkerService names the service a mod's started calls reach its workers
// through, with the worker's script as the method.
inline constexpr std::string_view kWorkerService = "modlock.worker";

// Worker runs a second store of a mod's interpreter module on a thread of its
// own, so bulk work that touches no game state, such as decoding a table,
// leaves the engine thread free. The worker's script gets one request at a
// time, in the order Post took them, and each answer ends the started
// service call that carried the request.
//
// The worker's script is a bundle that sets __modlock.event, as a mod's does.
// It receives the encoded Call "Start" once, then one Call "Request" for each
// posted payload, and answers with an encoded Reply whose response is the
// answer. It may call only Log, ReadFile and ReadFileBytes; every other call
// is refused, so it can reach neither the game nor another thread's state.
//
// Post and the destructor run on the engine thread. Everything else runs on
// the worker's thread, which also reads the script and the module and builds
// the store, so starting a worker never delays a frame.
class Worker {
 public:
  // ModuleSource returns the interpreter module that runs a worker's script.
  // The worker calls it once, on its own thread.
  using ModuleSource = std::function<std::expected<std::vector<uint8_t>, std::string>()>;

  // Worker starts a thread that loads the module from module, compiled in
  // runtime, and evaluates the file script of files. name labels its log
  // lines. runtime must outlive it.
  Worker(Runtime& runtime, ModuleSource module, std::string script, ModFiles files,
         std::string name);

  // The destructor stops the request running within one epoch tick and
  // refuses the requests still waiting. A worker still reading its files or
  // loading its store, or inside a ReadFile, finishes that first: a few
  // milliseconds for the store, and a file read's time for the rest.
  ~Worker();
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  // Post queues payload for the script and ends reply with the script's
  // answer, or with the error that kept it from answering.
  void Post(std::string payload, WasmReply reply);

  // Trapped reports whether the store stopped on a request, such as one that
  // ran out of time or memory. Its state is gone, but its script started
  // once, so a new worker can serve the next request.
  bool Trapped();

 private:
  // Request is one queued payload and the reply that ends its call.
  struct Request {
    std::string payload;
    WasmReply reply;
  };

  void Run(std::stop_token stop);
  void Log(const std::string& text) const;
  void Fail(const std::string& failure, bool trapped);

  Runtime& runtime_;
  const ModuleSource module_;
  const std::string script_;
  const ModFiles files_;
  const std::string name_;

  // mu_ guards queue_, failure_ and trapped_. A failed worker answers every
  // request with failure_; trapped_ says it failed on a request rather than
  // while starting.
  std::mutex mu_;
  std::condition_variable_any wake_;
  std::deque<Request> queue_;
  std::optional<std::string> failure_;
  bool trapped_ = false;

  // thread_ is last, so it stops before the state it uses is destroyed.
  std::jthread thread_;
};

}  // namespace modlock::wasm
