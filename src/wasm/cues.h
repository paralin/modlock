#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

#include "modlock/engine_host.h"
#include "src/wasm/effects/effects.pb.h"
#include "wasm/hook_sources.h"
#include "wasm/host_service.gen.h"
#include "wasm/module.h"

namespace modlock::wasm {

class GameServices;

// Cues keeps the particles and sounds a mod names from the players it names,
// while the events that start them still resolve. The shared effect filter
// reads it as a cue source while it lives and merges every mod's rules.
class Cues final : public Module, private CueSource, public CuesCalls {
 public:
  // services and engine outlive the cues. It registers as a cue source while
  // it lives.
  Cues(GameServices& services, EngineHost* engine);
  ~Cues() override;

  Cues(const Cues&) = delete;
  Cues& operator=(const Cues&) = delete;

  // HideCues replaces the mod's rules.
  std::expected<void, std::string> HideCues(const effects::HideCuesRequest& request) override;

  // Leave lets the slot's next player see and hear every cue.
  void Leave(int32_t slot) override;

  // Clear lets every player see and hear every cue.
  void Clear() override;

 private:
  bool Names(gameinterop::NativeEffectFilter::Kind kind) const override;
  uint64_t Skipped(const gameinterop::NativeEffectFilter::Cue& cue) const override;

  // Rule is one rule's source entity index, or -1 for any, and the slots it
  // keeps the cue from.
  struct Rule {
    int32_t source = -1;
    uint64_t viewers = 0;
  };

  using Rules = std::unordered_map<uint64_t, std::vector<Rule>>;

  GameServices& services_;
  EngineHost* engine_;
  // particles_ holds the rules by particle name and sounds_ by sound name
  // hash, so a cue costs one lookup.
  Rules particles_;
  Rules sounds_;
};

}  // namespace modlock::wasm
