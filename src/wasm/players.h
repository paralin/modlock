#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "modlock/gameinterop/pawn_observer.h"
#include "src/wasm/players/players.pb.h"
#include "wasm/module.h"

namespace modlock::wasm {

class GameServices;

// Players is a mod's view of the seated players and their heroes. It owns the
// pawn observer, which samples each player's hero once per frame; the modules
// that act on heroes reach it through Observer. A world change drops the
// observer's cached pawns, since the engine frees them with the world.
class Players final : public Module {
 public:
  // services outlives the players.
  explicit Players(GameServices& services);

  // Hero is one live hero this frame. Velocity is absent when the game's
  // motion field cannot be read.
  struct Hero {
    int32_t slot = 0;
    uint32_t handle = 0;
    void* pawn = nullptr;
    std::array<float, 3> feet{};
    std::optional<std::array<float, 3>> velocity;
  };

  // List returns every seated player, connected or away, with its identity,
  // readiness and presence.
  players::PlayersResponse List() const;

  // Pawn answers the player's hero, or no pawn for a slot without one or a
  // spectator; Pawns answers every live hero in slot order.
  players::PawnResponse Pawn(const players::PlayerRequest& request);
  players::PawnsResponse Pawns();

  // Live returns the observed pawn of slot for this frame.
  std::expected<gameinterop::PawnObserver::Sample, std::string> Live(int32_t slot);

  // LivePawn returns the pawn Live would observe for slot, without reading its
  // sample, for a call that only acts on the hero. The pointer is borrowed for
  // the call.
  std::expected<void*, std::string> LivePawn(int32_t slot) const;

  // SlotOfPawn returns the slot of the player whose hero the entity handle
  // names, or no value for any other entity.
  std::optional<int32_t> SlotOfPawn(uint32_t handle) const { return observer_.SlotOfPawn(handle); }

  // Heroes returns every live hero this frame.
  std::vector<Hero> Heroes();

  // Observer is the pawn observer the hero modules act through.
  gameinterop::PawnObserver& Observer() { return observer_; }

  const gameinterop::PawnObserver& Observer() const { return observer_; }

  // WorldReady prepares the observer's seams and the holding-idol state that
  // Pawn reads.
  void WorldReady() override;

  // WorldEnding forgets the observer's cached pawns.
  void WorldEnding() override;

 private:
  // FillPawn copies the player's sampled hero into pawn.
  void FillPawn(int32_t slot, const gameinterop::PawnObserver::Sample& sample, players::Pawn* pawn);

  GameServices& services_;
  gameinterop::PawnObserver observer_;
};

}  // namespace modlock::wasm
