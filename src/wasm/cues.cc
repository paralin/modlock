//go:build deps_only

#include "wasm/cues.h"

#include <utility>

#include "modlock/gameinterop/entity_abi.h"
#include "wasm/game.h"

namespace modlock::wasm {

using gameinterop::NativeEffectFilter;

Cues::Cues(GameServices& services, EngineHost* engine) : services_(services), engine_(engine) {
  services_.CueSources().Add(this);
}

Cues::~Cues() {
  services_.CueSources().Remove(this);
}

std::expected<void, std::string> Cues::HideCues(const effects::HideCuesRequest& request) {
  Rules particles;
  Rules sounds;
  for (const auto& rule : request.rules()) {
    if (rule.has_sound() == rule.has_particle_id()) {
      return std::unexpected("a cue rule names one sound or one particle");
    }
    Rule kept{.source = rule.has_source()
                            ? static_cast<int32_t>(gameinterop::EntityIndexOf(rule.source()))
                            : -1};
    for (const auto slot : rule.viewers()) {
      if (slot < 0 || slot >= gameinterop::kMaxPlayerSlots) {
        return std::unexpected("cue viewer slot " + std::to_string(slot) + " is out of range");
      }
      kept.viewers |= uint64_t{1} << slot;
    }
    if (rule.viewers().empty()) {
      kept.viewers = ~uint64_t{0};
    }
    if (rule.has_sound()) {
      sounds[NativeEffectFilter::SoundName(rule.sound())].push_back(kept);
    } else {
      particles[rule.particle_id()].push_back(kept);
    }
  }

  // The filter stays once installed; with no rules it forwards each cue
  // message unread.
  if (!particles.empty() || !sounds.empty()) {
    if (auto installed = services_.Cues(engine_); !installed) {
      return installed;
    }
  }
  particles_ = std::move(particles);
  sounds_ = std::move(sounds);
  return {};
}

void Cues::Leave(int32_t slot) {
  // A rule's viewers are the players in those slots now; the next player
  // there starts clear.
  for (auto* rules : {&particles_, &sounds_}) {
    for (auto& [name, kept] : *rules) {
      for (auto& rule : kept) {
        rule.viewers &= ~(uint64_t{1} << slot);
      }
    }
  }
}

void Cues::Clear() {
  particles_.clear();
  sounds_.clear();
}

bool Cues::Names(NativeEffectFilter::Kind kind) const {
  return !(kind == NativeEffectFilter::Kind::kSound ? sounds_ : particles_).empty();
}

uint64_t Cues::Skipped(const NativeEffectFilter::Cue& cue) const {
  const auto& rules = cue.kind == NativeEffectFilter::Kind::kSound ? sounds_ : particles_;
  const auto found = rules.find(cue.name);
  if (found == rules.end()) {
    return 0;
  }
  uint64_t skipped = 0;
  for (const auto& rule : found->second) {
    if (rule.source < 0 || rule.source == cue.source) {
      skipped |= rule.viewers;
    }
  }
  return skipped;
}

}  // namespace modlock::wasm
