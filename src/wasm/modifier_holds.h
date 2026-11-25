#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modlock/gameinterop/ability_modifier.h"
#include "src/wasm/abilities/abilities.pb.h"
#include "src/wasm/entities/entities.pb.h"
#include "src/wasm/fields/fields.pb.h"
#include "wasm/module.h"
#include "wasm/players.h"
#include "wasm/world.h"

namespace modlock::wasm {

class GameServices;

// ModifierHolds answers a mod's modifier calls and keeps what it holds: the
// modifiers each player's hero or each entity keeps until released, given back
// each frame after the game removes them, and the modifier states set again
// each frame after the game recomputes them. Every hold ends with the world,
// a held player's leaving, or Clear, and Clear takes held modifiers off their
// entities, so an endless modifier never outlives the mod.
class ModifierHolds final : public Module {
 public:
  // services, players and world outlive the holds.
  ModifierHolds(GameServices& services, Players& players, World& world)
      : services_(services), players_(players), world_(world) {}

  ModifierHolds(const ModifierHolds&) = delete;
  ModifierHolds& operator=(const ModifierHolds&) = delete;

  std::expected<void, std::string> HoldModifier(const abilities::HoldModifierRequest& request);
  std::expected<void, std::string> GiveModifier(const abilities::GiveModifierRequest& request);
  std::expected<void, std::string> HoldEntityModifier(
      const abilities::HoldEntityModifierRequest& request);
  std::expected<void, std::string> GiveEntityModifier(
      const abilities::GiveEntityModifierRequest& request);
  std::expected<abilities::ModifiersResponse, std::string> Modifiers(
      const entities::EntityRequest& request);
  std::expected<fields::FieldResponse, std::string> ReadModifierField(
      const fields::ReadModifierFieldRequest& request);
  std::expected<abilities::ActiveResponse, std::string> ModifierState(
      const abilities::ModifierStateRequest& request);
  std::expected<void, std::string> HoldModifierState(
      const abilities::HoldModifierStateRequest& request);

  // KeepState holds state on the entity with handle from the next frame on,
  // as free-for-all holds friendly fire.
  void KeepState(uint32_t handle, const std::string& state);

  // ForgetState drops the hold KeepState took, for an entity that is gone.
  void ForgetState(uint32_t handle, const std::string& state);

  // ReleaseState drops the hold and clears state on the entity when it was
  // held.
  void ReleaseState(uint32_t handle, const std::string& state);

  // Apply sets every held state again, since the game recomputes states each
  // frame and on teleport, and forgets a hold whose entity is gone. It gives
  // held modifiers back to the living heroes and entities that lost them. A
  // teleport calls it at once, and Frame once a frame.
  void Apply();

  void WorldReady() override;
  void Frame() override;

  void WorldEnding() override;
  void Leave(int32_t slot) override;
  void Clear() override;

 private:
  // ModifierSource is the ability that applies a modifier keyed key to slot's
  // live hero: the ability before the key's slash when the hero owns it, and
  // otherwise, as for a top-level modifier, the hero's first ability.
  std::expected<uint32_t, std::string> ModifierSource(int32_t slot, std::string_view key);

  // EntityModifiers returns the address of each modifier entity holds, in
  // the game's order, or none for an entity without a modifier property.
  std::expected<std::vector<unsigned char*>, std::string> EntityModifiers(void* entity);

  // EnsureHeldModifier resolves key with no expiry once for all active holds.
  std::expected<void, std::string> EnsureHeldModifier(const std::string& key);

  // EntityAbilityHandle returns the owned ability handle when handle is a
  // player's hero that owns key's parent ability, or kInvalidEHandleIndex.
  uint32_t EntityAbilityHandle(uint32_t handle, std::string_view key);

  // RenewModifier gives slot's living hero the held modifier when it lacks
  // it. A dead or absent hero waits for its return.
  std::expected<void, std::string> RenewModifier(int32_t slot, const std::string& key);

  // RenewEntityModifier gives handle's live entity the held modifier when it
  // lacks it. When initial is true, a dead entity fails the hold.
  std::expected<void, std::string> RenewEntityModifier(uint32_t handle, const std::string& key,
                                                       bool initial);

  // ReleaseModifier ends slot's hold on key and restores the modifier's
  // duration once nothing holds it.
  void ReleaseModifier(int32_t slot, const std::string& key);

  // ReleaseEntityModifier ends handle's hold on key and restores the modifier's
  // duration once nothing holds it.
  void ReleaseEntityModifier(uint32_t handle, const std::string& key);

  // RestoreUnusedModifier restores and forgets key once no slot or entity holds
  // it.
  void RestoreUnusedModifier(const std::string& key);

  // ReleaseModifiers ends every modifier hold.
  void ReleaseModifiers();

  GameServices& services_;
  Players& players_;
  // world_ removes the held modifiers from their entities on Clear.
  World& world_;
  // states_ is the modifier states held, by entity handle and name; Frame
  // sets them again.
  std::set<std::pair<uint32_t, std::string>> states_;
  // held_slots_ and held_entities_ are the modifiers held, by player slot or
  // entity handle and modifier key; modifiers_ resolves each key once with no
  // expiry while anything holds it.
  std::set<std::pair<int32_t, std::string>> held_slots_;
  std::set<std::pair<uint32_t, std::string>> held_entities_;
  std::map<std::string, gameinterop::AbilityModifier, std::less<>> modifiers_;
};

}  // namespace modlock::wasm
