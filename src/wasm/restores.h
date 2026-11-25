#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/wasm/npcs/npcs.pb.h"
#include "src/wasm/players/players.pb.h"
#include "wasm/hero_restore.h"
#include "wasm/heroes.h"
#include "wasm/modifier_holds.h"
#include "wasm/module.h"
#include "wasm/npc_restore.h"
#include "wasm/players.h"
#include "wasm/world.h"

namespace modlock::wasm {

class GameServices;

// Restores gives heroes and units the state a mod saved for them, over
// several frames each, and reports each restore once it ends.
class Restores final : public Module {
 public:
  // Restored is the result of one player's hero restore; an empty error
  // means the target held.
  struct Restored {
    int32_t slot = 0;
    std::string error;
  };

  // services, players, world, modifiers and heroes outlive the restores.
  Restores(GameServices& services, Players& players, World& world, ModifierHolds& modifiers,
           Heroes& heroes);

  Restores(const Restores&) = delete;
  Restores& operator=(const Restores&) = delete;

  std::expected<void, std::string> RestoreHero(const players::RestoreHeroRequest& request);
  std::expected<void, std::string> RestoreNpcs(const npcs::RestoreNpcsRequest& request);

  // TakeRestored returns the hero restores that ended since the last call.
  std::vector<Restored> TakeRestored() { return std::exchange(restored_, {}); }

  // TakeNpcsRestored returns the result of the unit restore that ended since
  // the last call: an empty string when the units hold their targets.
  std::optional<std::string> TakeNpcsRestored() { return std::exchange(npcs_restored_, {}); }

  // Frame advances every hero restore and the unit restore by one frame and
  // queues the ones that ended.
  void Frame() override;

  // WorldEnding drops the running restores; the results stay for the mod.
  void WorldEnding() override;

  // Leave drops the slot's running hero restore.
  void Leave(int32_t slot) override;

  // Clear drops the running restores and their unread results.
  void Clear() override;

 private:
  // Apply performs one stage of a hero restore's writes.
  bool Apply(const HeroRestore::Sample& sample, const RestoreTarget& target,
             HeroRestore::Stage stage);

  GameServices& services_;
  Players& players_;
  World& world_;
  ModifierHolds& modifiers_;
  Heroes& heroes_;
  // running_ are the running hero restores by player slot, ticked once per
  // frame; restored_ queues the ones that ended for the mod.
  std::map<int32_t, HeroRestore> running_;
  uint64_t frames_ = 0;
  std::vector<Restored> restored_;
  // npc_restore_ is the running unit restore; npcs_restored_ is the result
  // of the last one that ended, until the mod receives it.
  std::optional<NpcRestore> npc_restore_;
  std::optional<std::string> npcs_restored_;
};

}  // namespace modlock::wasm
