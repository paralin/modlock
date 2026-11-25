//go:build deps_only

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>

#include "wasm/game.h"

namespace modlock::wasm {

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
  if (std::ranges::any_of(
          voices_, [&](const VoiceSource* voice) { return voice->Silences(concept_value); })) {
    return false;
  }

  // Otherwise ask each mod about the player whose hero speaks.
  const auto handle = gameinterop::ReferenceHandleOf(speaker);
  return !handle || std::ranges::none_of(voices_, [&](const VoiceSource* voice) {
    return voice->SilencesSpeaker(*handle, concept_value);
  });
}

}  // namespace modlock::wasm
