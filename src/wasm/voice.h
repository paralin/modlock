#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <set>
#include <string>

#include "src/wasm/effects/effects.pb.h"
#include "wasm/hook_sources.h"
#include "wasm/module.h"
#include "wasm/players.h"

namespace modlock::wasm {

class GameServices;

// Voice keeps heroes from speaking the voice concepts a mod silences, for
// every hero or one player's. The shared voice hook reads it as a voice
// source while it lives and merges every mod's silences.
class Voice final : public Module, private VoiceSource {
 public:
  // services and players outlive the voice. It registers as a voice source
  // while it lives.
  Voice(GameServices& services, Players& players);
  ~Voice() override;

  Voice(const Voice&) = delete;
  Voice& operator=(const Voice&) = delete;

  // The calls replace the mod's silences.
  std::expected<void, std::string> SilenceVoice(const effects::SilenceVoiceRequest& request);
  std::expected<void, std::string> SilencePlayerVoice(
      const effects::SilencePlayerVoiceRequest& request);

  // Leave lets the slot's hero speak again.
  void Leave(int32_t slot) override;

  // Clear lets every hero speak again.
  void Clear() override;

 private:
  bool Silences(int32_t concept_value) const override;
  bool SilencesSpeaker(uint32_t speaker, int32_t concept_value) const override;

  // Silence installs the voice hook while the mod silences any concept, then
  // reports the concept names the game did not know, from unknown.
  std::expected<void, std::string> Silence(const std::string& unknown);

  GameServices& services_;
  Players& players_;
  // silenced_ is the voice concepts the mod keeps every hero from speaking,
  // and silenced_players_ those it keeps from one player's hero, by slot.
  std::set<int32_t> silenced_;
  std::map<int32_t, std::set<int32_t>> silenced_players_;
};

}  // namespace modlock::wasm
