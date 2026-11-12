#include "modlock/gameinterop/voice_hook.h"

#include <cstdint>

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

// kMaxConcept bounds a plausible CitadelConcept_t value; the game defines
// about a hundred.
constexpr int64_t kMaxConcept = 1024;

#if defined(_WIN32)
VoiceHook::Handler* g_handler = nullptr;
safetyhook::InlineHook* g_hook = nullptr;

// SpeakThunk stands in for the speak call. The arguments after the concept,
// the criteria and the speech filter, pass through unread at full width. The
// call reports whether the line played in its low byte; a refused line
// reports that none did.
uintptr_t SpeakThunk(void* speaker, int32_t concept_value, uintptr_t modifiers, uintptr_t filter,
                     uintptr_t output, uintptr_t allies) {
  if (speaker && !(*g_handler)(speaker, concept_value)) {
    return 0;
  }
  return g_hook->call<uintptr_t>(speaker, concept_value, modifiers, filter, output, allies);
}
#endif

}  // namespace

std::expected<int32_t, std::string> VoiceConcept(const ModuleImage& server, std::string_view name) {
  auto value = EnumeratorValue(server, name, kMaxConcept);
  if (!value) {
    return std::unexpected("voice concept " + value.error());
  }
  return static_cast<int32_t>(*value);
}

struct VoiceHook::Impl {
  Handler handler;
#if defined(_WIN32)
  safetyhook::InlineHook hook;

  ~Impl() {
    // Only the installed hook clears the shared state.
    if (g_hook != &hook) {
      return;
    }
    hook.reset();
    g_hook = nullptr;
    g_handler = nullptr;
  }
#endif
};

VoiceHook::VoiceHook(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

VoiceHook::VoiceHook(VoiceHook&&) noexcept = default;
VoiceHook& VoiceHook::operator=(VoiceHook&&) noexcept = default;
VoiceHook::~VoiceHook() = default;

std::expected<VoiceHook, std::string> VoiceHook::Install(const ModuleImage& server,
                                                         Handler handler) {
#if defined(_WIN32)
  // Install once, with a handler.
  if (g_hook || !handler) {
    return std::unexpected("voice hook already installed or empty");
  }
  auto target = ResolveSignature(server, "voice.speak");
  if (!target) {
    return std::unexpected(target.error());
  }

  // Create the hook disabled, publish the state the thunk reads, then enable.
  auto impl = std::make_unique<Impl>();
  impl->handler = std::move(handler);
  impl->hook = safetyhook::create_inline(*target, reinterpret_cast<void*>(&SpeakThunk),
                                         safetyhook::InlineHook::StartDisabled);
  if (!impl->hook) {
    return std::unexpected("cannot create native voice hook");
  }
  g_handler = &impl->handler;
  g_hook = &impl->hook;
  if (!impl->hook.enable()) {
    return std::unexpected("cannot enable native voice hook");
  }
  return VoiceHook(std::move(impl));

#else
  // Other platforms have no native server.
  (void)server;
  (void)handler;
  return std::unexpected("native voice filtering requires Windows");
#endif
}

}  // namespace modlock::gameinterop
