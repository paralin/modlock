#pragma once

#include <google/protobuf/repeated_field.h>

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>

#include "modlock/engine_host.h"
#include "modlock/gameinterop/world_entities.h"
#include "modlock/render/world_effect_game_factory.h"
#include "modlock/render/world_text_game_factory.h"
#include "src/wasm/entities/entities.pb.h"
#include "src/wasm/fields/fields.pb.h"
#include "src/wasm/npcs/npcs.pb.h"
#include "src/wasm/objects/objects.pb.h"
#include "src/wasm/players/players.pb.h"
#include "src/wasm/wasm.pb.h"
#include "wasm/hook_sources.h"
#include "wasm/host_service.gen.h"
#include "wasm/module.h"
#include "wasm/visibility.h"

namespace modlock::wasm {

class GameServices;

// World is what a mod placed in the loaded world and who may see it: its
// objects (models, text, particles and fog), the entities, units and pickups
// it created, the bots it added, and their audiences. It builds the world's
// entity operations on first use and lends them to the other modules as their
// WorldSource. Clear and WorldEnding remove everything the mod placed, so a
// stopped or reloaded mod leaves nothing behind, and the transmit hook reads
// the audiences through its TransmitSource while the world lives.
class World final : public Module, public WorldSource, private TransmitSource, public WorldCalls {
 public:
  // services and engine outlive the world; the host owns the engine.
  World(GameServices& services, EngineHost* engine);
  ~World();

  World(const World&) = delete;
  World& operator=(const World&) = delete;

  // Entities returns this world's entity operations, building them on first
  // use.
  std::expected<gameinterop::WorldEntities*, std::string> Entities() override;

  // Loaded returns the world's entity operations when they are built, or
  // null, for a release that has nothing to undo in a world never touched.
  gameinterop::WorldEntities* Loaded() { return entities_ ? &*entities_ : nullptr; }

  // StartContentAddon names the content addon of the mod that starts, which
  // CreateEntity writes in place of {addon} in a text key value, and
  // CreateModel and CreateParticle in a resource path, so a world panel names
  // its layout as file://{resources}/layout/{addon}/panel.xml and a model its
  // resource as models/{addon}/pawn.vmdl.
  void StartContentAddon(std::string addon) { addon_ = std::move(addon); }

  // Addon returns the running mod's content addon.
  const std::string& Addon() const { return addon_; }

  // WithAddon returns text with the mod's content addon in place of each
  // {addon}.
  std::string WithAddon(std::string text) const;

  // Departing reports whether a bot the mod removed still holds its slot.
  // The engine finishes the disconnect of a client it is processing after
  // the call returns, so the slot can stay held for a moment.
  bool Departing();

  // The object calls.
  std::expected<objects::ObjectResponse, std::string> CreateModel(
      const objects::ModelOptions& request) override;
  std::expected<objects::ObjectResponse, std::string> CreateText(
      const objects::TextOptions& request) override;
  std::expected<objects::ObjectResponse, std::string> CreateParticle(
      const objects::ParticleOptions& request) override;
  std::expected<objects::ObjectResponse, std::string> CreateFog(
      const objects::FogOptions& request) override;
  std::expected<void, std::string> MoveObject(const objects::MoveObjectRequest& request) override;
  std::expected<void, std::string> SetText(const objects::SetTextRequest& request) override;
  std::expected<void, std::string> SetObjectHidden(
      const objects::SetObjectHiddenRequest& request) override;
  std::expected<void, std::string> ShowObjectOnlyTo(
      const objects::ShowObjectOnlyToRequest& request) override;
  std::expected<void, std::string> ShowObjectToAll(const objects::ObjectRequest& request) override;
  std::expected<void, std::string> RemoveObject(const objects::ObjectRequest& request) override;
  std::expected<entities::EntityResponse, std::string> ObjectEntity(
      const objects::ObjectRequest& request) override;

  // The bot calls.
  std::expected<players::BotResponse, std::string> AddBot(
      const players::BotOptions& request) override;
  std::expected<void, std::string> RemoveBot(const players::PlayerRequest& request) override;

  // Precache loads heroes and resources before the mod places them.
  std::expected<void, std::string> Precache(const PrecacheOptions& request) override;

  // The entity calls.
  std::expected<entities::EntityResponse, std::string> CreateEntity(
      const entities::EntityOptions& request) override;
  std::expected<entities::AliveResponse, std::string> FireInput(
      const entities::FireInputRequest& request) override;
  std::expected<entities::AliveResponse, std::string> RemoveEntity(
      const entities::EntityRequest& request) override;
  std::expected<void, std::string> ShowEntityOnlyTo(
      const entities::ShowEntityOnlyToRequest& request) override;
  std::expected<void, std::string> ShowEntityToAll(const entities::EntityRequest& request) override;
  std::expected<void, std::string> MoveEntity(const entities::MoveEntityRequest& request) override;
  std::expected<void, std::string> PushEntity(const entities::PushEntityRequest& request) override;
  std::expected<entities::EntitiesResponse, std::string> FindEntities(
      const entities::FindEntitiesRequest& request) override;
  std::expected<entities::EntitiesResponse, std::string> FindEntitiesByName(
      const entities::FindEntitiesByNameRequest& request) override;
  std::expected<entities::CountResponse, std::string> RemoveEntities(
      const entities::RemoveEntitiesRequest& request) override;
  std::expected<void, std::string> ClearMap() override;

  // The unit and pickup calls.
  std::expected<npcs::NpcResponse, std::string> SpawnNpc(const npcs::NpcOptions& request) override;
  std::expected<npcs::NpcStateResponse, std::string> ReadNpc(
      const npcs::NpcRequest& request) override;
  std::expected<entities::AliveResponse, std::string> MoveNpc(
      const npcs::MoveNpcRequest& request) override;
  std::expected<entities::AliveResponse, std::string> SetNpcHealth(
      const npcs::SetNpcHealthRequest& request) override;
  std::expected<entities::AliveResponse, std::string> RemoveNpc(
      const npcs::NpcRequest& request) override;
  std::expected<entities::PickupResponse, std::string> CreatePickup(
      const entities::CreatePickupRequest& request) override;
  std::expected<entities::AliveResponse, std::string> PickupPresent(
      const entities::PickupRequest& request) override;
  std::expected<entities::AliveResponse, std::string> RemovePickup(
      const entities::PickupRequest& request) override;

  // Frame drops the audiences of gone entities, releases ended effects and
  // finishes the units spawned this frame together.
  void Frame() override;

  // WorldEnding removes what the mod placed and forgets the world's
  // entities, which the engine frees with the world.
  void WorldEnding() override;

  // Leave forgets what the slot's player was shown or kept from.
  void Leave(int32_t slot) override;

  // Clear removes what the mod placed and puts back the scale of each entity
  // it rescaled but did not create.
  void Clear() override;

 private:
  // Object is one world object the mod created.
  using Object =
      std::variant<std::unique_ptr<render::WorldEffect>, std::unique_ptr<render::WorldTextEntity>>;

  // Withhold removes from set the objects and entities the mod keeps from
  // set's player.
  void Withhold(gameinterop::TransmitSet& set, const Sources<CameraSource>& cameras) override;

  // Keep stores a created object and returns its response.
  objects::ObjectResponse Keep(Object object);

  // ObjectHandle returns the entity handle of the mod's object id now, or
  // nothing.
  std::optional<uint32_t> ObjectHandle(uint32_t id) const;

  // WriteBeforeSpawn returns the preparation that stores fields on an entity
  // the world creates, before it spawns and is networked. fields must outlive
  // the creation.
  gameinterop::WorldEntities::Prepare WriteBeforeSpawn(
      const google::protobuf::RepeatedPtrField<fields::FieldWrite>& fields);

  // RemoveWorld removes the objects, units and bots.
  void RemoveWorld();

  // Depart records that the bot in slot was disconnected.
  void Depart(int32_t slot);

  GameServices& services_;
  EngineHost* engine_;
  // addon_ is the running mod's content addon.
  std::string addon_;
  // entities_ creates and steers entities in the loaded world, built on first
  // use and dropped with the world.
  std::optional<gameinterop::WorldEntities> entities_;
  std::map<uint32_t, Object> objects_;
  uint32_t next_object_ = 1;
  // fog_ is the world's fog controller, which the fog objects need; it is
  // created with the first and removed after the objects.
  std::unique_ptr<render::WorldEffect> fog_;
  // visibility_ is who receives the mod's objects and entities.
  Visibility visibility_;
  // created_ are the entities, units and pickups the mod created, and
  // spawned_ is true until the units' placement finishes.
  std::set<uint32_t> created_;
  bool spawned_ = false;
  // scaled_ maps each entity the mod rescaled but did not create, such as a
  // hero, to the scale it had before the mod's first SetAbsScale; Clear puts
  // it back, so the next mod in the game starts from the game's own sizes.
  std::map<uint32_t, float> scaled_;
  // bots_ is the slots of the bots the mod added.
  std::set<int32_t> bots_;
  // departing_ maps each removed bot's slot to its connection generation
  // until the slot empties or another connection takes it.
  std::map<int32_t, uint32_t> departing_;
};

}  // namespace modlock::wasm
