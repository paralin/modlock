#include "host_app/windows/engine_log.h"

#include <cstdio>
#include <cstring>

#include "host_app/windows/import_patch.h"

namespace modlock::host_app {
namespace {

// EngineLog is a tier0 logging listener (tier0/logging.h ILoggingListener)
// that writes each engine message to stdout as it arrives. The slots after
// Log keep the interface's empty defaults.
class EngineLog {
 public:
  virtual void Log(const void* /*context*/, const char* message) {
    std::fwrite(message, 1, std::strlen(message), stdout);
    std::fflush(stdout);
  }
  virtual void OnFlush() {}
  virtual void OnChannelRegistered(int /*channel*/) {}
  virtual void OnChannelVerbosityChanged(int /*channel*/) {}
  virtual void OnChannelFlagsChanged(int /*channel*/) {}
};

using RegisterFn = void (*)(EngineLog* listener);
using PushFn = void (*)(bool thread_local_state, bool clear_state);
using ResetFn = void (*)();

// The tier0 functions are written once before the engine starts and read by
// every thread that changes the logging state.
EngineLog engine_log;
RegisterFn register_listener = nullptr;
PushFn original_push = nullptr;
ResetFn original_reset = nullptr;

// PushLoggingState pushes the engine's new logging state and joins it when it
// starts empty; a copied state already holds the listener.
void PushLoggingState(bool thread_local_state, bool clear_state) {
  original_push(thread_local_state, clear_state);
  if (clear_state) register_listener(&engine_log);
}

// ResetCurrentLoggingState empties the current state and rejoins it.
void ResetCurrentLoggingState() {
  original_reset();
  register_listener(&engine_log);
}

}  // namespace

std::expected<void, std::string> ListenEngineLog(HMODULE engine2) {
  HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
  if (!tier0) return std::unexpected("engine log: tier0.dll is not mapped");
  register_listener = reinterpret_cast<RegisterFn>(
      reinterpret_cast<void*>(GetProcAddress(tier0, "LoggingSystem_RegisterLoggingListener")));
  if (!register_listener) {
    return std::unexpected("engine log: tier0.dll has no LoggingSystem_RegisterLoggingListener");
  }
  register_listener(&engine_log);

  auto push = PatchImport(engine2, "tier0.dll", "LoggingSystem_PushLoggingState", 0,
                          reinterpret_cast<void*>(&PushLoggingState));
  if (!push) return std::unexpected("engine log: engine2.dll: " + push.error());
  original_push = reinterpret_cast<PushFn>(*push);
  auto reset = PatchImport(engine2, "tier0.dll", "LoggingSystem_ResetCurrentLoggingState", 0,
                           reinterpret_cast<void*>(&ResetCurrentLoggingState));
  if (!reset) return std::unexpected("engine log: engine2.dll: " + reset.error());
  original_reset = reinterpret_cast<ResetFn>(*reset);
  return {};
}

}  // namespace modlock::host_app
