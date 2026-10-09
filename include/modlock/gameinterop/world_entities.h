#pragma once

#include <array>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/keyvalues.h"

namespace modlock::gameinterop {

class NativeDamage;

// WorldEntities creates, reconciles and removes entities through the native
// entity system. Resolve and all operations run on the engine thread after
// world initialization.
class MODLOCK_API WorldEntities {
 public:
  // Calls binds the native entity lifecycle and resolved schema offsets. Native
  // interfaces and schema storage must outlive this engine-thread adapter.
  struct Calls {
    std::expected<void*, std::string> (*entity_system)() = nullptr;
    void* schema = nullptr;
    KeyValuesCalls key_values;
    void* (*find_class)(void* system, const char* designer_name, void* unused) = nullptr;
    void* (*create)(void*, const char*, int) = nullptr;
    void (*queue)(void*, void*, void*) = nullptr;
    void (*execute)(void*) = nullptr;
    void (*remove)(void*) = nullptr;
    void* (*definition)(int32_t, uint32_t) = nullptr;
    bool (*accept_input)(void* entity, const char* input, void* activator, void* caller,
                         void* value, int output_id, void* unknown) = nullptr;
    std::array<size_t, 7> offsets{};
  };
  explicit WorldEntities(Calls calls) : calls_(std::move(calls)) {}

  struct Target {
    std::string designer_name;
    uint32_t subclass_id;
    int32_t team;
    std::array<float, 3> position;
    std::array<float, 3> facing;
    std::array<float, 3> velocity;
    int32_t health;
    int32_t max_health;
    std::optional<uint32_t> lane;
  };
  struct Sample {
    uint32_t handle;
    Target state;
  };
  static std::expected<WorldEntities, std::string> Resolve(const ModuleImage& server,
                                                           void* schema_system);
  std::expected<std::vector<Sample>, std::string> Read() const;
  // Restore retains matching structures, replaces moving NPCs, and removes
  // structures absent from targets. FinishRestore completes their placement
  // after one native simulation frame; a later Read verifies the world.
  std::expected<void, std::string> Restore(std::span<const Target> targets);
  // FinishRestore applies recorded state once initialization has run, using
  // the native identities retained by Restore. Call on the next engine frame.
  std::expected<void, std::string> FinishRestore();

  // ClearAuthored kills interfering NPCs and removes native round objectives without
  // kill rewards. It preserves scenery and urn delivery triggers. Exact replay
  // restoration never calls this authored-world operation.
  std::expected<void, std::string> ClearAuthored(const NativeDamage& damage);
  // Remove deletes every live entity with this exact designer name through
  // UTIL_Remove, without damage, kill rewards or the console cheat gate. It
  // returns how many entities were queued for removal.
  std::expected<size_t, std::string> Remove(std::string_view designer_name);
  // Find returns the handle of every live entity with this exact designer
  // name.
  std::expected<std::vector<uint32_t>, std::string> Find(std::string_view designer_name);

  // Spawn adds one NPC beside the existing world through CreateEntity and
  // returns its handle. A health of zero keeps the subclass default. Call
  // FinishSpawns on the next engine frame; it reapplies placement and health
  // to spawns that are still alive.
  // Prepare, when set, runs on the created entity before it spawns, for
  // fields the game reads only while spawning; its failure cancels the spawn.
  using Prepare = std::function<std::expected<void, std::string>(void* entity)>;
  std::expected<uint32_t, std::string> Spawn(const Target& target, const Prepare& prepare = {});
  void FinishSpawns();
  // CreateEntity creates any designer name the server knows, applies
  // key_values as its spawn key values, and returns its handle. The target's
  // subclass, team and placement apply as for Spawn; its health and velocity
  // are ignored. Prepare runs as for Spawn. It refuses an ability, item or
  // weapon, which only a hero holds.
  std::expected<uint32_t, std::string> CreateEntity(const Target& target,
                                                    std::span<const EntityKeyValue> key_values,
                                                    const Prepare& prepare = {});
  // FireInput sends input to one live entity, as a map's output would, with
  // value as its parameter, absent for an input that takes none. The value
  // must have the type the input reads; the game does not convert it. The
  // activator, a handle, is the entity that caused the input. It returns
  // false when the entity is gone and an error when the activator is gone or
  // the entity refuses the input.
  std::expected<bool, std::string> FireInput(uint32_t handle, const std::string& input,
                                             const std::optional<EntityValue>& value,
                                             std::optional<uint32_t> activator = {});
  // RemoveEntity deletes one live entity through UTIL_Remove without rewards.
  // It returns false when the handle no longer names one.
  std::expected<bool, std::string> RemoveEntity(uint32_t handle);
  // Exists reports whether the handle still names a live entity.
  std::expected<bool, std::string> Exists(uint32_t handle) const;
  // ReadNpc samples one live NPC by handle; nullopt once it is gone or dead.
  std::expected<std::optional<Sample>, std::string> ReadNpc(uint32_t handle) const;
  // Move teleports one live NPC, for modes that steer units themselves each
  // frame; false when it is gone.
  std::expected<bool, std::string> Move(uint32_t handle, const std::array<float, 3>& position,
                                        const std::array<float, 3>& facing,
                                        const std::array<float, 3>& velocity);
  // SetHealth sets one live NPC's health and maximum; false when it is gone.
  std::expected<bool, std::string> SetHealth(uint32_t handle, int32_t health, int32_t max_health);
  // SubclassId is the VData subclass hash for an npc_units entry name, such as
  // "trooper_melee" or "npc_boss_tier1".
  static uint32_t SubclassId(std::string_view vdata_name);

 private:
  std::expected<Sample, std::string> ReadEntity(void* entity, std::string name) const;
  // LaneOffset returns where an entity whose class walks a lane, such as a
  // trooper, stores it, or nullopt for one that walks none.
  std::optional<size_t> LaneOffset(void* entity) const;
  std::expected<void*, std::string> Create(const Target& target,
                                           std::span<const EntityKeyValue> key_values = {},
                                           const Prepare& prepare = {});
  std::expected<void, std::string> Apply(void* entity, const Target& target) const;
  Calls calls_;
  std::vector<Sample> pending_;
  std::vector<Sample> spawned_;
  // lanes_ caches LaneOffset by schema class.
  mutable std::unordered_map<const void*, std::optional<size_t>> lanes_;
};

}  // namespace modlock::gameinterop
