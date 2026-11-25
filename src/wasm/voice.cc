//go:build deps_only

#include "wasm/voice.h"

#include <string>
#include <utility>

#include "wasm/game.h"

namespace modlock::wasm {

Voice::Voice(GameServices& services, Players& players) : services_(services), players_(players) {
  services_.VoiceSources().Add(this);
}

Voice::~Voice() {
  services_.VoiceSources().Remove(this);
}

void Voice::Leave(int32_t slot) {
  silenced_players_.erase(slot);
}

void Voice::Clear() {
  silenced_.clear();
  silenced_players_.clear();
}

std::expected<void, std::string> Voice::SilenceVoice(const effects::SilenceVoiceRequest& request) {
  auto [concepts, unknown] = services_.Silenced(request.concepts());
  silenced_ = std::move(concepts);
  return Silence(unknown);
}

std::expected<void, std::string> Voice::SilencePlayerVoice(
    const effects::SilencePlayerVoiceRequest& request) {
  auto [concepts, unknown] = services_.Silenced(request.concepts());
  if (concepts.empty()) {
    silenced_players_.erase(request.player());
  } else {
    silenced_players_[request.player()] = std::move(concepts);
  }
  return Silence(unknown);
}

std::expected<void, std::string> Voice::Silence(const std::string& unknown) {
  // The hook stays once installed; an empty silence costs it one lookup.
  if (!silenced_.empty() || !silenced_players_.empty()) {
    if (auto voice = services_.Voice(); !voice) {
      return voice;
    }
  }
  if (!unknown.empty()) {
    return std::unexpected("unknown voice concepts: " + unknown);
  }
  return {};
}

bool Voice::Silences(int32_t concept_value) const {
  return silenced_.contains(concept_value);
}

bool Voice::SilencesSpeaker(uint32_t speaker, int32_t concept_value) const {
  if (silenced_players_.empty()) {
    return false;
  }
  const auto slot = players_.Observer().SlotOfPawn(speaker);
  if (!slot) {
    return false;
  }
  const auto found = silenced_players_.find(*slot);
  return found != silenced_players_.end() && found->second.contains(concept_value);
}

}  // namespace modlock::wasm
