#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "modlock/engine_host.h"
#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/wasm_host.h"
#include "proto/modlock/control/control.pb.h"

namespace modlock::host_app {

// ControlLink connects modlock-host to the program that started it, such as
// modlock dev, with the framing control.proto describes. The host dials the
// controller's loopback address, streams mod starts, logs, failures, server
// readiness, player joins and leaves and mod interfaces, and takes reloads.
// Requests run on the engine thread through RunRequests. When the controller
// disconnects, events are dropped and the host keeps running.
class ControlLink final : public WasmHostObserver, public gameinterop::ConnectionEventSink {
 public:
  ~ControlLink() override;
  ControlLink(const ControlLink&) = delete;
  ControlLink& operator=(const ControlLink&) = delete;

  // Dial connects to address, a host and port, and starts the link.
  [[nodiscard]] static std::expected<std::unique_ptr<ControlLink>, std::string> Dial(
      std::string_view address);

  // Watch reports server readiness and player joins from engine, from the
  // next world load on. It runs once, before the engine starts; engine must
  // outlive the link.
  [[nodiscard]] std::expected<void, std::string> Watch(EngineHost& engine);

  // RunRequests applies the requests received since the last call to mods. It
  // runs on the engine thread.
  void RunRequests(WasmHost& mods);

  void Started(std::string_view mod, bool reloaded) override;
  void Logged(std::string_view mod, std::string_view text) override;
  void Failed(std::string_view mod, std::string_view error) override;
  void Ui(std::string_view mod, int32_t slot, const ui::Change& change) override;
  void OnConnected(int32_t slot, uint64_t xuid, bool bot, const char* name) override;
  void OnDisconnecting(int32_t slot, uint64_t xuid) override;

 private:
  explicit ControlLink(intptr_t socket);

  void Push(control::HostEvent event);
  void Send(std::stop_token stop);
  void Read();

  intptr_t socket_;
  std::mutex mu_;
  std::condition_variable_any wake_;
  // events_ waits for the sender; closed_ is true once the controller
  // disconnected and events are dropped.
  std::deque<control::HostEvent> events_;
  bool closed_ = false;
  // requests_ holds the requests to run; pending_ is true while it has any.
  std::vector<control::ControlRequest> requests_;
  std::atomic<bool> pending_ = false;
  // world_ and connections_ follow the engine for the link's lifetime;
  // joining_ is true once connections_ subscribed.
  Subscription world_;
  Subscription connections_;
  bool joining_ = false;
  // reader_ queues requests and sender_ sends events; the destructor stops
  // both before the connection closes.
  std::jthread reader_;
  std::jthread sender_;
};

}  // namespace modlock::host_app
