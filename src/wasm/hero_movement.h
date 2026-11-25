#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "modlock/gameinterop/command_hook.h"
#include "src/wasm/movement/movement.pb.h"
#include "wasm/hook_sources.h"
#include "wasm/host_service.gen.h"
#include "wasm/modifier_holds.h"
#include "wasm/module.h"
#include "wasm/roster.h"
#include "wasm/world.h"

namespace modlock::wasm {

class GameServices;

// HeroMovement answers a mod's calls that move its players' heroes: teleporting,
// warping onto a zipline, moving, steering along a recorded path, driving a
// bot with commands, and setting a velocity. It samples the heroes whose
// movement the mod watches, and tells the host-wide movement and command
// hooks which heroes it steers and drives.
class HeroMovement final : public Module, private MoverSource, public MovementCalls {
 public:
  // MovementFact is one movement fact the game announced for the hero with
  // the pawn handle; an executed ability carries the ability's handle, and
  // a completed interaction, with no action, the interaction.
  struct MovementFact {
    uint32_t pawn = 0;
    movement::MovementAction action = movement::MOVEMENT_ACTION_UNKNOWN;
    uint32_t ability = 0;
    std::optional<movement::Interaction> interaction;
  };

  // services, players, world and modifiers outlive the movement. It
  // registers as a mover source while it lives, and on destruction lets the
  // movement hook go when no other mod moves a hero.
  HeroMovement(GameServices& services, Roster& players, World& world, ModifierHolds& modifiers);
  ~HeroMovement() override;

  HeroMovement(const HeroMovement&) = delete;
  HeroMovement& operator=(const HeroMovement&) = delete;

  std::expected<void, std::string> Teleport(const movement::TeleportRequest& request) override;
  std::expected<void, std::string> Zipline(const movement::ZiplineRequest& request) override;
  std::expected<void, std::string> MovePlayer(const movement::MovePlayerRequest& request) override;
  std::expected<void, std::string> Steer(const movement::SteerRequest& request) override;
  std::expected<void, std::string> Drive(const movement::DriveRequest& request) override;
  std::expected<void, std::string> SetVelocity(
      const movement::SetVelocityRequest& request) override;
  std::expected<void, std::string> WatchMovement(
      const movement::WatchMovementRequest& request) override;

  // Moving reports whether the mod watches any player's movement.
  bool Moving() const { return !movers_.empty(); }

  // Samples returns a sample of each watched, live hero, with the facts that
  // name its pawn.
  std::vector<movement::MovementSample> Samples(std::span<const MovementFact> facts);

  // WorldEnding forgets the steered and driven heroes, which go with the
  // world.
  void WorldEnding() override;

  // Leave forgets the slot's watch, steering and drive.
  void Leave(int32_t slot) override;

  // Clear stops watching, steering and driving every hero, and lets the
  // movement hook go when no other mod moves one.
  void Clear() override;

 private:
  gameinterop::CommandHook::Command* DriveOf(int32_t slot) override;
  movement::Steering* SteerOf(int32_t slot) override;
  bool Moves() const override;
  void Aim(void* pawn, const std::array<float, 3>& angles) override;

  // NearestZiplineNode returns where the zipline node nearest pawn stands
  // among those on lane that pawn's team may ride.
  std::expected<std::array<float, 3>, std::string> NearestZiplineNode(void* pawn, int32_t lane);

  GameServices& services_;
  Roster& players_;
  // world_ finds the zipline nodes.
  World& world_;
  // modifiers_ gives a teleported hero its held modifiers back at once.
  ModifierHolds& modifiers_;
  // movers_ is the players whose movement the mod watches.
  std::set<int32_t> movers_;
  // steers_ maps each steered player to the command its hero replays.
  std::map<int32_t, movement::Steering> steers_;
  // drives_ maps each driven bot's slot to the command its hero runs.
  std::map<int32_t, gameinterop::CommandHook::Command> drives_;
};

}  // namespace modlock::wasm
