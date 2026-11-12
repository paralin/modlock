//go:build deps_only

#include <iostream>
#include <string>
#include <utility>

#include "wasm/game.h"

namespace modlock::wasm {

// The voice calls replace the mod's silences; the shared hook merges every
// mod's.

std::expected<void, std::string> Game::SilenceVoice(const effects::SilenceVoiceRequest& request) {
  auto [concepts, unknown] = services_.Silenced(request.concepts());
  silenced_ = std::move(concepts);
  return Silence(unknown);
}

std::expected<void, std::string> Game::SilencePlayerVoice(
    const effects::SilencePlayerVoiceRequest& request) {
  auto [concepts, unknown] = services_.Silenced(request.concepts());
  if (concepts.empty()) {
    silenced_players_.erase(request.player());
  } else {
    silenced_players_[request.player()] = std::move(concepts);
  }
  return Silence(unknown);
}

std::expected<void, std::string> Game::Silence(const std::string& unknown) {
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

std::pair<std::set<int32_t>, std::string> GameServices::Silenced(
    const google::protobuf::RepeatedPtrField<std::string>& names) {
  std::set<int32_t> concepts;
  std::string unknown;
  for (const auto& name : names) {
    // Answer a concept resolved before from the cache.
    if (auto found = voice_concepts_.find(name); found != voice_concepts_.end()) {
      concepts.insert(found->second);
      continue;
    }

    // Resolve it in the server's enumerators and cache it.
    auto image = Image();
    auto value = image ? gameinterop::VoiceConcept(**image, name)
                       : std::expected<int32_t, std::string>(std::unexpected(image.error()));
    if (!value) {
      std::cerr << "mod voice: " << value.error() << '\n';
      unknown += unknown.empty() ? name : ", " + name;
      continue;
    }
    voice_concepts_.emplace(name, *value);
    concepts.insert(*value);
  }
  return {std::move(concepts), std::move(unknown)};
}

std::expected<void, std::string> GameServices::Voice() {
  // Install the voice hook once.
  if (voice_) {
    return {};
  }
  auto image = Image();
  if (!image) {
    return std::unexpected(image.error());
  }
  auto hook = gameinterop::VoiceHook::Install(
      **image,
      [this](void* speaker, int32_t concept_value) { return Speaks(speaker, concept_value); });
  if (!hook) {
    return std::unexpected(hook.error());
  }
  voice_ = std::move(*hook);
  return {};
}

bool GameServices::Speaks(void* speaker, int32_t concept_value) {
  // A concept silenced for everyone needs no speaker.
  bool personal = false;
  for (const auto* game : games_) {
    if (game->silenced_.contains(concept_value)) {
      return false;
    }
    personal |= !game->silenced_players_.empty();
  }
  if (!personal) {
    return true;
  }

  // Otherwise find the player whose hero speaks, once.
  const auto handle = gameinterop::ReferenceHandleOf(speaker);
  if (!handle) {
    return true;
  }
  std::optional<int32_t> slot;
  for (const auto* game : games_) {
    if (game->silenced_players_.empty()) {
      continue;
    }
    if (!slot) {
      slot = game->observer_.SlotOfPawn(*handle);
      if (!slot) {
        return true;
      }
    }
    const auto found = game->silenced_players_.find(*slot);
    if (found != game->silenced_players_.end() && found->second.contains(concept_value)) {
      return false;
    }
  }
  return true;
}

}  // namespace modlock::wasm
