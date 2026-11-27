#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "modlock/gameinterop/command_hook.h"
#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/native_effect_filter.h"
#include "modlock/gameinterop/native_trace.h"
#include "modlock/gameinterop/transmit_hook.h"
#include "src/wasm/combat/combat.pb.h"
#include "src/wasm/movement/movement.pb.h"

// The host-wide hooks in GameServices serve every running mod at once. Each
// reads the mods through one of the source interfaces below, which a mod's
// game registers while it runs, so a hook depends on what it reads and never
// on the game that holds it. Everything runs on the engine thread.

namespace modlock::wasm {

// Sources is the set of one kind of source registered with a host-wide hook,
// in the order they registered.
template <typename Source>
class Sources {
 public:
  // Add registers source until Remove.
  void Add(Source* source) { sources_.push_back(source); }

  void Remove(Source* source) { std::erase(sources_, source); }

  // Contains reports whether source is still registered, as after a call
  // into a mod that may have stopped it.
  bool Contains(const Source* source) const {
    return std::ranges::find(sources_, source) != sources_.end();
  }

  auto begin() const { return sources_.begin(); }

  auto end() const { return sources_.end(); }

 private:
  std::vector<Source*> sources_;
};

// MoverSource answers the command and movement hooks: the players a mod
// drives with commands or steers along a recorded path.
class MoverSource {
 public:
  // DriveOf returns the command the mod drives the player in slot with, or
  // null.
  virtual gameinterop::CommandHook::Command* DriveOf(int32_t slot) = 0;

  // SteerOf returns the recording the mod replays on the player in slot, or
  // null.
  virtual movement::Steering* SteerOf(int32_t slot) = 0;

  // Moves reports whether the mod drives, steers or watches any player's
  // movement, so the movement hook must stay installed.
  virtual bool Moves() const = 0;

  // Aim sets pawn's eye angles, as the drive or steering the mod gave it.
  virtual void Aim(void* pawn, const std::array<float, 3>& angles) = 0;

 protected:
  ~MoverSource() = default;
};

// CameraSource answers which hero each watching player's camera follows.
class CameraSource {
 public:
  // WatchedSlot returns the slot of the player whose hero the player in slot
  // watches, or none.
  virtual std::optional<int32_t> WatchedSlot(int32_t slot) const = 0;

 protected:
  ~CameraSource() = default;
};

// WatchesInside reports whether the player in slot watches, through any
// mod's camera, a hero among the slots set in apart.
inline bool WatchesInside(const Sources<CameraSource>& cameras, int32_t slot, uint64_t apart) {
  for (const auto* camera : cameras) {
    const auto target = camera->WatchedSlot(slot);
    if (target && *target >= 0 && *target < gameinterop::kMaxPlayerSlots &&
        ((apart >> *target) & 1)) {
      return true;
    }
  }
  return false;
}

// TransmitSource answers the transmit hook: what a mod hides from a player.
class TransmitSource {
 public:
  // Withhold removes from set the entities the mod hides from set's player.
  // cameras are every mod's, since a watcher outside a mod's apart players
  // may follow one of them through another mod's camera.
  virtual void Withhold(gameinterop::TransmitSet& set, const Sources<CameraSource>& cameras) = 0;

 protected:
  ~TransmitSource() = default;
};

// ProjectileSource answers the projectile impact hook.
class ProjectileSource {
 public:
  // HeroMotion is a live hero's pawn and its velocity.
  struct HeroMotion {
    void* pawn = nullptr;
    std::array<float, 3> velocity{};
  };

  // Watching returns the impact event when the mod watches entity's impacts.
  virtual std::optional<combat::ImpactEvent> Watching(void* entity,
                                                      const gameinterop::TraceResult& contact) = 0;

  // KeepsMomentum reports whether the mod keeps heroes' motion through the
  // game's explosions.
  virtual bool KeepsMomentum() const = 0;

  // Motions returns every live hero's motion this frame.
  virtual std::vector<HeroMotion> Motions() = 0;

  // Impacted delivers a watched impact to the mod, inside the game's impact.
  virtual void Impacted(const combat::ImpactEvent& event) = 0;

 protected:
  ~ProjectileSource() = default;
};

// VoiceSource answers the voice hook: the concepts a mod keeps heroes from
// speaking.
class VoiceSource {
 public:
  // Silences reports whether the mod silences concept_value for every hero.
  virtual bool Silences(int32_t concept_value) const = 0;

  // SilencesSpeaker reports whether the mod silences concept_value for the
  // hero with the pawn handle speaker.
  virtual bool SilencesSpeaker(uint32_t speaker, int32_t concept_value) const = 0;

 protected:
  ~VoiceSource() = default;
};

// CueSource answers the effect filter: the particles and sounds a mod keeps
// from players.
class CueSource {
 public:
  // Names reports whether any of the mod's rules names a cue of kind.
  virtual bool Names(gameinterop::NativeEffectFilter::Kind kind) const = 0;

  // Skipped returns the slots, one bit each, the mod keeps cue from.
  virtual uint64_t Skipped(const gameinterop::NativeEffectFilter::Cue& cue) const = 0;

 protected:
  ~CueSource() = default;
};

}  // namespace modlock::wasm
