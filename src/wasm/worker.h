#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
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
// the worker's thread, which also builds the store, so loading the module
// never delays a frame.
class Worker {
 public:
  // Worker starts a thread that loads module, compiled in runtime, and
  // evaluates source. name labels its log lines. runtime must outlive it.
  Worker(Runtime& runtime, std::span<const uint8_t> module, std::string source, ModFiles files,
         std::string name);

  // The destructor stops the request running within one epoch tick and
  // refuses the requests still waiting.
  ~Worker();
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  // Post queues payload for the script and ends reply with the script's
  // answer, or with the error that kept it from answering.
  void Post(std::string payload, WasmReply reply);

 private:
  // Request is one queued payload and the reply that ends its call.
  struct Request {
    std::string payload;
    WasmReply reply;
  };

  void Run(std::stop_token stop);
  void Log(const std::string& text) const;
  void Fail(const std::string& failure);

  Runtime& runtime_;
  const std::vector<uint8_t> module_;
  const std::string source_;
  const ModFiles files_;
  const std::string name_;

  // mu_ guards queue_ and failure_. A worker that failed to start answers
  // every request with failure_.
  std::mutex mu_;
  std::condition_variable_any wake_;
  std::deque<Request> queue_;
  std::optional<std::string> failure_;

  // thread_ is last, so it stops before the state it uses is destroyed.
  std::jthread thread_;
};

}  // namespace modlock::wasm
