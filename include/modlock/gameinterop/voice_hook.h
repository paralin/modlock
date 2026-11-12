#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// VoiceConcept reads the value of one CitadelConcept_t enumerator by its name,
// such as "TLK_CITADEL_GOLD_GAINED", from the server's enumerator records.
// Game updates may renumber the concepts, so callers never hard-code them.
[[nodiscard]] MODLOCK_API std::expected<int32_t, std::string> VoiceConcept(
    const ModuleImage& server, std::string_view name);

// VoiceHook wraps the server call through which an entity speaks a voice
// concept: every hero voice line, from a kill to a reminder to spend souls,
// starts there with the speaker and its CitadelConcept_t. The handler runs
// synchronously on the engine thread and returns whether the speaker may
// speak; a refused line is dropped before the game's response rules choose
// one, as if none matched. Destruction removes the detour.
class MODLOCK_API VoiceHook {
 public:
  using Handler = std::function<bool(void* speaker, int32_t concept_value)>;

  static std::expected<VoiceHook, std::string> Install(const ModuleImage& server, Handler handler);
  VoiceHook(VoiceHook&&) noexcept;
  VoiceHook& operator=(VoiceHook&&) noexcept;
  ~VoiceHook();
  VoiceHook(const VoiceHook&) = delete;
  VoiceHook& operator=(const VoiceHook&) = delete;

 private:
  struct Impl;
  explicit VoiceHook(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
