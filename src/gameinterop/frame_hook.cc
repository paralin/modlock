#include "modlock/gameinterop/frame_hook.h"

#include <iostream>
#include <string>
#include <utility>

#include "modlock/gameinterop/frame_timing.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/thunk_owner.h"

namespace modlock::gameinterop {
namespace {

// The thunk state: one hook may exist at a time, so the replacement reads its
// original target and callback from these slots. Only the server thread
// touches g_frame_timing.
void* g_game_frame_original = nullptr;
FrameCallback g_on_frame;
FrameTiming g_frame_timing;

// ClearThunkState drops the singleton thunk inputs. Callers restore their
// dispatch-table entry first: while an entry still points at the thunk, a null
// original would swallow every engine frame.
void ClearThunkState() {
  g_game_frame_original = nullptr;
  g_on_frame = nullptr;
}

using GameFrameFn = void (*)(void* self, bool simulating, bool first_tick, bool last_tick);

// GameFrameThunk runs the engine's own GameFrame first, then modlock's frame
// work on the same server thread, and logs their timing once per window.
void GameFrameThunk(void* self, bool simulating, bool first_tick, bool last_tick) {
  const auto start = FrameTiming::Clock::now();
  if (g_game_frame_original != nullptr) {
    reinterpret_cast<GameFrameFn>(g_game_frame_original)(self, simulating, first_tick, last_tick);
  }
  if (g_on_frame) {
    g_on_frame();
  }
  if (auto summary = g_frame_timing.Record(start, FrameTiming::Clock::now())) {
    std::cout << FormatFrameTiming(*summary) << std::endl;
  }
}

}  // namespace

EngineFrameHook::EngineFrameHook(ThunkOwner owner) : owner_(std::move(owner)) {}

std::expected<EngineFrameHook, std::string> EngineFrameHook::Install(FrameCallback on_frame) {
  const auto server = ResolveEngineInterface(L"server.dll", kServerInterfaceVersion);
  if (!server) return std::unexpected(server.error());
  auto hook =
      VtableSlotHook::Install(*server, kGameFrameSlot, reinterpret_cast<void*>(&GameFrameThunk));
  if (!hook.has_value()) {
    return std::unexpected(hook.error());
  }
  g_game_frame_original = hook->Original();
  g_on_frame = std::move(on_frame);
  return EngineFrameHook(ThunkOwner(std::move(*hook), &ClearThunkState));
}

}  // namespace modlock::gameinterop
