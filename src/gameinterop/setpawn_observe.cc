#include "modlock/gameinterop/setpawn_observe.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

#if defined(_WIN32)

// SetPawnObservation is default-off and read-only. The thunk runs the engine
// original first, then records one bounded line per call - the engine's own
// pawn binding proceeds exactly as if no hook existed.
safetyhook::InlineHook* g_hook = nullptr;

// Rate-safety counters: the only mutable state the observation carries.
size_t g_lines_emitted = 0;
size_t g_line_budget = 256;
bool g_overflow_logged = false;

// OverflowMarker emits the single typed overflow marker once the budget is
// exhausted; later calls log nothing.
void OverflowMarker() {
  g_overflow_logged = true;
  std::fprintf(stderr,
               "[modlock] setpawn observe: OVERFLOW line budget reached; further "
               "SetPawn calls are not logged\n");
  std::fflush(stderr);
}

// LogLine writes one bounded evidence line to durable stderr.
void LogLine(const char* text) {
  // Emit lines up to the budget, then one overflow marker.
  if (g_overflow_logged) {
    return;
  }
  if (g_lines_emitted >= g_line_budget) {
    OverflowMarker();
    return;
  }
  ++g_lines_emitted;
  std::fprintf(stderr, "%s\n", text);
  std::fflush(stderr);
}

#if defined(_MSC_VER) && !defined(__clang__)
#define MODLOCK_RETURN_ADDRESS _ReturnAddress()
#else
#define MODLOCK_RETURN_ADDRESS __builtin_return_address(0)
#endif

// SetPawnThunk runs the engine original first, then records the call. The
// ABI is void(CBasePlayerController* this, CBasePlayerPawn* pawn, bool, bool,
// bool, bool): the four bools are one-byte Windows x64 ABI values.
void __fastcall SetPawnThunk(void* controller, void* pawn, uint8_t retain_old_pawn_team,
                             uint8_t copy_movement_state, uint8_t allow_team_mismatch,
                             uint8_t preserve_movement_state) {
  // The engine's own pawn binding runs first, so the observation cannot
  // change engine behaviour - including when the flags are nonzero.
  g_hook->call(controller, pawn, retain_old_pawn_team, copy_movement_state, allow_team_mismatch,
               preserve_movement_state);

  // The caller return address is the instruction after the call that entered
  // SetPawn; it resolves to a caller RVA in a static disassembly of server.dll.
  void* ret = MODLOCK_RETURN_ADDRESS;

  // Format the observation and log it.
  char line[192];

  // Hex fields are explicit 0x%llx, not %p: the documented contract names
  // 0x-prefixed hex, and %p renders differently on MSVC (zero-padded) than on
  // glibc, which would make the same evidence line platform-dependent.
  std::snprintf(
      line, sizeof(line),
      "[modlock] setpawn observe: controller=0x%llx pawn=0x%llx flags=%u,%u,%u,%u "
      "caller=0x%llx",
      reinterpret_cast<unsigned long long>(controller), reinterpret_cast<unsigned long long>(pawn),
      static_cast<unsigned>(retain_old_pawn_team), static_cast<unsigned>(copy_movement_state),
      static_cast<unsigned>(allow_team_mismatch), static_cast<unsigned>(preserve_movement_state),
      reinterpret_cast<unsigned long long>(ret));
  LogLine(line);
}

#endif  // _WIN32

}  // namespace

struct SetPawnObservation::Impl {
#if defined(_WIN32)
  safetyhook::InlineHook hook;

  ~Impl() {
    if (g_hook == &hook) {
      hook.reset();
      g_hook = nullptr;
    }
  }
#endif
};

SetPawnObservation::SetPawnObservation(SetPawnObservation&& other) noexcept = default;
SetPawnObservation& SetPawnObservation::operator=(SetPawnObservation&& other) noexcept = default;
SetPawnObservation::~SetPawnObservation() = default;

SetPawnObservation::SetPawnObservation(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

std::expected<SetPawnObservation, std::string> SetPawnObservation::Install(
    const ModuleImage& server) {
#if defined(_WIN32)
  // Stage 1: locate SetPawn. Zero or multiple matches fail loud: an engine
  // update that shifts the pattern must stop here, not mis-hook.
  auto target = ResolveSignature(server, "controller.set-pawn");
  if (!target.has_value()) {
    return std::unexpected(target.error());
  }

  // Stage 2: install disabled, arm the thunk state, then enable. A
  // partially-initialised observation never dispatches into an unset
  // original.
  auto created = safetyhook::create_inline(*target, reinterpret_cast<void*>(&SetPawnThunk),
                                           safetyhook::InlineHook::StartDisabled);
  if (!created) {
    return std::unexpected("inline hook on SetPawn failed");
  }

  // Arm the thunk state and enable the hook.
  auto impl = std::make_unique<Impl>();
  impl->hook = std::move(created);
  g_hook = &impl->hook;
  if (auto enabled = impl->hook.enable(); !enabled) {
    impl->hook.reset();
    g_hook = nullptr;
    return std::unexpected("SetPawn observation hook could not be enabled");
  }

  // Report where the hook armed.
  std::fprintf(stderr, "[modlock] setpawn observe: armed on server.dll+0x%zx; logging every call\n",
               reinterpret_cast<std::uintptr_t>(*target) - server.base());
  std::fflush(stderr);
  return SetPawnObservation(std::move(impl));
#else
  (void)server;
  return std::unexpected("the SetPawn observation requires the Windows host build");
#endif
}

}  // namespace modlock::gameinterop
