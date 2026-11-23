#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "src/wasm/profile/profile.pb.h"
#include "wasm/futures.h"
#include "wasm/instance.h"
#include "wasm/mod_thread.h"
#include "wasm/runtime.h"

namespace modlock::wasm {

// kBusy is the reason a decision gives when the mod could not take it now.
inline constexpr std::string_view kBusy = "the mod is busy";

// Sandbox is the running mod as the plugin sees it: either an instance that
// runs each event on the calling thread, or a ModThread that runs them on a
// thread of its own. Both deliver one event at a time through Deliver; the
// plugin tells them apart only to run a window each tick and to take the
// events that finished in it.
//
// An empty sandbox holds no mod.
class Sandbox {
 public:
  Sandbox() = default;

  // Load compiles module and runs its initialization on the calling thread,
  // as an instance or, when concurrent holds settings, as a ModThread with them. host_call
  // answers the mod's calls and runs only on the calling thread.
  [[nodiscard]] static std::expected<Sandbox, std::string> Load(
      Runtime& runtime, std::span<const uint8_t> module, const Limits& limits,
      Instance::HostCall host_call, std::optional<ModThreadSettings> concurrent);

  // operator bool reports whether the sandbox holds a mod.
  explicit operator bool() const { return instance_ || thread_; }

  // Concurrent reports whether the mod runs on a thread of its own.
  bool Concurrent() const { return thread_ != nullptr; }

  // Reset stops the mod and empties the sandbox. A mod on its own thread is
  // interrupted, a hop it waits on answered with an error, and its thread
  // joined.
  void Reset();

  // Deliver sends call to the mod. An instance runs it now and returns the
  // reply. A mod on a thread of its own treats the events by what the plugin
  // does with the answer:
  //   - Start runs now, before the thread starts.
  //   - Damage, Command, Ping and Serve decide something the engine needs
  //     now, so they run now when the mod is idle and return kBusy as an
  //     error otherwise, for the caller to take its default.
  //   - Every other event runs now on the calling thread when the mod is idle
  //     with nothing queued, and is queued otherwise; either way its reply comes
  //     back from Completed.
  [[nodiscard]] std::expected<Reply, std::string> Deliver(const Call& call);

  // Failure returns why the mod failed, or nullopt while it runs.
  [[nodiscard]] std::optional<std::string> Failure() const;

  // SlowReport describes where the time of the event Deliver ran went when it
  // ran past the slow limit, and is empty otherwise. An event queued for a
  // mod on its own thread reports through Completed.
  [[nodiscard]] std::optional<std::string> SlowReport() const;

  // Report and Describe say the same about the last event an instance ran,
  // slow or not.
  [[nodiscard]] std::string Report() const;
  [[nodiscard]] profile::SlowFrame Describe() const;

  // Window lets a mod on its own thread run until it is idle or deadline
  // passes, with frame delivered first when there is one. See ModThread::Window.
  WindowStats Window(std::chrono::steady_clock::time_point deadline,
                     std::optional<Call> frame = std::nullopt);

  // Completed returns the events that finished on the mod's thread since the
  // last call.
  [[nodiscard]] std::vector<Completion> Completed();

  // Flush runs the calls an instance's mod submitted, on the calling thread
  // and in the order submitted, and returns the Completed event that
  // delivers their replies, or nullopt when none ended. A mod on its own
  // thread has its calls run by Window and its Completed event posted there,
  // so Flush returns nullopt.
  [[nodiscard]] std::optional<CompletedEvent> Flush();

 private:
  // Submitted holds the calls an instance's mod submitted until Flush runs
  // them.
  struct Submitted;

  std::unique_ptr<Instance> instance_;
  std::unique_ptr<ModThread> thread_;
  // submitted_ is set for an instance; shared, since its host call holds it.
  std::shared_ptr<Submitted> submitted_;
  // decided_ is the last decision a mod on its own thread took.
  std::optional<Completion> decided_;
};

}  // namespace modlock::wasm
