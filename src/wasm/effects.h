#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "modlock/engine_host.h"
#include "src/wasm/combat/combat.pb.h"
#include "src/wasm/effects/effects.pb.h"
#include "src/wasm/players/players.pb.h"
#include "src/wasm/wasm.pb.h"
#include "wasm/players.h"

namespace modlock::wasm {

class GameServices;

// Effects carries the calls that act on the game at once and keep nothing:
// server commands, messages, screen effects, sounds, heals, damage, kills and
// traces. It holds no state, so it runs no hooks and is not one of the game's
// modules.
class Effects final {
 public:
  // services, engine and players outlive the effects.
  Effects(GameServices& services, EngineHost* engine, Players& players);

  Effects(const Effects&) = delete;
  Effects& operator=(const Effects&) = delete;

  // ServerCommand runs one server console line.
  std::expected<void, std::string> ServerCommand(const ServerCommandRequest& request);

  // The messages show text to one player, or to every player.
  std::expected<void, std::string> Chat(const effects::ChatRequest& request);
  std::expected<void, std::string> CenterText(const effects::CenterTextRequest& request);
  std::expected<void, std::string> Announce(const effects::AnnounceRequest& request);

  // The screen effects show or clear an effect on a player's screen.
  std::expected<void, std::string> ScreenEffect(const effects::ScreenEffectRequest& request);
  std::expected<void, std::string> ClearScreenEffect(
      const effects::ClearScreenEffectRequest& request);

  // The sounds start a sound on a player's hero or an entity, change it or
  // stop it.
  std::expected<effects::SoundResponse, std::string> Sound(const effects::SoundRequest& request);
  std::expected<effects::SoundResponse, std::string> EmitSound(
      const effects::EmitSoundRequest& request);
  std::expected<void, std::string> AdjustSound(const effects::AdjustSoundRequest& request);
  std::expected<void, std::string> StopSound(const effects::StopSoundRequest& request);

  // Heal raises a live hero's health, at most to its maximum.
  std::expected<void, std::string> Heal(const players::HealRequest& request);

  // Damage and KillEntity hurt or kill a hero or unit through the game's
  // damage path.
  std::expected<void, std::string> Damage(const combat::HitOptions& request);
  std::expected<void, std::string> KillEntity(const combat::KillOptions& request);

  // Trace casts a line through the world's collision.
  std::expected<combat::TraceResponse, std::string> Trace(const combat::TraceOptions& request);

 private:
  // ScreenOwner returns the entity index a screen effect for slot belongs to.
  std::expected<int32_t, std::string> ScreenOwner(int32_t slot);

  // HumanSlots returns the slot bits of the connected human players, whose
  // clients can hear a sound.
  static uint64_t HumanSlots();

  // PlaySound starts a sound on the source entity index, shaped and
  // addressed by options.
  std::expected<effects::SoundResponse, std::string> PlaySound(
      const std::string& sound, int32_t source, const effects::SoundOptions& options);

  // Combatant resolves a hero or unit that can safely receive damage events.
  std::expected<void*, std::string> Combatant(uint32_t handle);

  GameServices& services_;
  // engine_ outlives the effects; the host owns it.
  EngineHost* engine_;
  Players& players_;
};

}  // namespace modlock::wasm
