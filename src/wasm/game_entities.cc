//go:build deps_only

#include <algorithm>
#include <array>
#include <deque>
#include <utility>
#include <vector>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/keyvalues.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

namespace {

// Value converts an entity value to the world's form, or nothing when it has
// none. The result borrows value's text.
std::optional<gameinterop::EntityValue> Value(const entities::EntityValue& value) {
  switch (value.value_case()) {
    case entities::EntityValue::kBoolean:
      return value.boolean();
    case entities::EntityValue::kInteger:
      return value.integer();
    case entities::EntityValue::kNumber:
      return value.number();
    case entities::EntityValue::kText:
      return std::string_view(value.text());
    case entities::EntityValue::kVector: {
      const auto [x, y, z] = Floats(value.vector());
      return gameinterop::KeyValueVector{x, y, z};
    }
    case entities::EntityValue::kColor: {
      const uint32_t color = value.color();
      return gameinterop::KeyValueColor{
          static_cast<uint8_t>(color >> 24), static_cast<uint8_t>(color >> 16),
          static_cast<uint8_t>(color >> 8), static_cast<uint8_t>(color)};
    }
    case entities::EntityValue::VALUE_NOT_SET:
      break;
  }
  return std::nullopt;
}

// addonToken stands for the mod's content addon in a text value or path.
constexpr std::string_view addonToken = "{addon}";

// KeyValues converts spawn key values to the world's form, writing the mod's
// content addon in place of each {addon} in a text value. The result borrows
// the text of pairs, and of filled, which holds each text value it rewrote.
std::expected<std::vector<gameinterop::EntityKeyValue>, std::string> KeyValues(
    const google::protobuf::RepeatedPtrField<entities::KeyValue>& pairs, const Game& game,
    std::deque<std::string>& filled) {
  // Convert each pair, failing on a pair with no value.
  std::vector<gameinterop::EntityKeyValue> result;
  result.reserve(pairs.size());
  for (const auto& pair : pairs) {
    // Convert the pair's value.
    auto value = Value(pair.value());
    if (!value) {
      return std::unexpected("key value " + pair.key() + " has no value");
    }

    // Name the mod's content addon in a text value.
    const auto* text = std::get_if<std::string_view>(&*value);
    if (text && text->find(addonToken) != std::string_view::npos) {
      *value = std::string_view(filled.emplace_back(game.WithAddon(std::string(*text))));
    }
    result.push_back({.key = pair.key(), .value = *value});
  }
  return result;
}

// Alive answers whether the entity a call acted on is still there.
std::expected<entities::AliveResponse, std::string> Alive(std::expected<bool, std::string> alive) {
  if (!alive) {
    return std::unexpected(alive.error());
  }
  entities::AliveResponse response;
  response.set_alive(*alive);
  return response;
}

}  // namespace

std::string Game::WithAddon(std::string text) const {
  if (addon_.empty()) {
    return text;
  }
  for (auto at = text.find(addonToken); at != std::string::npos;
       at = text.find(addonToken, at + addon_.size())) {
    text.replace(at, addonToken.size(), addon_);
  }
  return text;
}

std::expected<objects::ObjectResponse, std::string> Game::CreateModel(
    const objects::ModelOptions& request) {
  // Resolve the effect factory.
  auto effects = services_.Effects();
  if (!effects) {
    return std::unexpected(effects.error());
  }

  // Create the model and keep it as one of the mod's objects.
  render::WorldModelSettings settings{
      .resource = WithAddon(request.resource()),
      .origin = request.position(),
      .angles = Floats(request.facing()),
      .scale = request.scale() == 0 ? 1 : request.scale(),
      // The factory reads red from the low byte, as 0xAABBGGRR.
      .color_rgba = request.color() == 0 ? UINT32_MAX : std::byteswap(request.color()),
      .glow = request.glow(),
      .solid = request.solid(),
  };
  auto model = (*effects)->CreateModel(settings);
  if (!model) {
    return std::unexpected(model.error());
  }
  return Keep(std::move(*model));
}

std::expected<objects::ObjectResponse, std::string> Game::CreateText(
    const objects::TextOptions& request) {
  // Resolve the text factory.
  auto text = services_.Text();
  if (!text) {
    return std::unexpected(text.error());
  }

  // Create the text and keep it as one of the mod's objects.
  render::WorldTextStyle style{
      .font_size = request.font_size() == 0 ? 100 : request.font_size(),
      // The entity takes its color as 0xAABBGGRR.
      .color_abgr = request.color() == 0 ? UINT32_MAX : std::byteswap(request.color()),
      .face_camera = request.face_camera(),
      .scale = request.scale() == 0 ? 1 : request.scale(),
      .justify_horizontal = static_cast<std::uint32_t>(request.align()),
      .justify_vertical = static_cast<std::uint32_t>(request.vertical_align()),
  };
  auto entity = (*text)->Create(request.text(), request.position(), request.facing(), style);
  if (!entity) {
    return std::unexpected(entity.error());
  }
  return Keep(std::move(*entity));
}

std::expected<void, std::string> Game::MoveObject(const objects::MoveObjectRequest& request) {
  auto found = objects_.find(request.object());
  if (found == objects_.end()) {
    return std::unexpected("no object has that identifier");
  }
  if (auto* model = std::get_if<std::unique_ptr<render::WorldEffect>>(&found->second)) {
    std::optional<std::array<float, 3>> angles;
    if (request.has_facing()) {
      angles = Floats(request.facing());
    }
    (*model)->Move(request.position(), angles);
  } else {
    std::get<std::unique_ptr<render::WorldTextEntity>>(found->second)
        ->SetOrigin(request.position(), request.facing());
  }
  return {};
}

std::expected<void, std::string> Game::SetText(const objects::SetTextRequest& request) {
  // Find the object and check it is text.
  auto found = objects_.find(request.object());
  if (found == objects_.end()) {
    return std::unexpected("no object has that identifier");
  }
  auto* text = std::get_if<std::unique_ptr<render::WorldTextEntity>>(&found->second);
  if (text == nullptr) {
    return std::unexpected("the object is not text");
  }

  // Replace the words.
  (*text)->SetMessage(request.text());
  return {};
}

std::expected<void, std::string> Game::SetObjectHidden(
    const objects::SetObjectHiddenRequest& request) {
  // Showing the object again drops it from the player's hidden set.
  if (!objects_.contains(request.object())) {
    return std::unexpected("no object has that identifier");
  }
  if (!request.hidden()) {
    visibility_.Unhide(request.player(), request.object());
    return {};
  }

  // The engine sends only what its transmit pass allows, so hiding needs the
  // shared transmit hook.
  if (auto transmit = services_.Transmit(); !transmit) {
    return transmit;
  }
  visibility_.Hide(request.player(), request.object());
  return {};
}

std::expected<void, std::string> Game::RemoveObject(const objects::ObjectRequest& request) {
  // Find the object.
  auto found = objects_.find(request.object());
  if (found == objects_.end()) {
    return std::unexpected("no object has that identifier");
  }

  // Forget who may see it, then remove it from the world.
  visibility_.RemoveObject(request.object(), ObjectHandle(request.object()));
  std::visit([](auto& handle) { handle->Remove(); }, found->second);
  objects_.erase(found);
  return {};
}

std::expected<void, std::string> Game::ShowObjectOnlyTo(
    const objects::ShowObjectOnlyToRequest& request) {
  // Find the object.
  if (!objects_.contains(request.object())) {
    return std::unexpected("no object has that identifier");
  }

  // Collect the audience's slot bits.
  const auto slots = Visibility::Audience(request.players());
  if (!slots) {
    return std::unexpected(slots.error());
  }

  // The engine sends only what its transmit pass allows, so an audience
  // needs the shared transmit hook.
  if (auto transmit = services_.Transmit(); !transmit) {
    return transmit;
  }
  visibility_.ShowObjectOnlyTo(request.object(), *slots);
  return {};
}

std::expected<void, std::string> Game::ShowObjectToAll(const objects::ObjectRequest& request) {
  if (!objects_.contains(request.object())) {
    return std::unexpected("no object has that identifier");
  }
  visibility_.ShowObjectToAll(request.object());
  return {};
}

std::expected<entities::EntityResponse, std::string> Game::ObjectEntity(
    const objects::ObjectRequest& request) {
  // Find the object and check it is a model or particle.
  auto found = objects_.find(request.object());
  if (found == objects_.end()) {
    return std::unexpected("no object has that identifier");
  }
  auto* effect = std::get_if<std::unique_ptr<render::WorldEffect>>(&found->second);
  if (effect == nullptr) {
    return std::unexpected("the object is text, which has no entity");
  }

  // Answer its live entity.
  auto handle = (*effect)->Handle();
  if (!handle) {
    return std::unexpected("the object is no longer live");
  }
  entities::EntityResponse response;
  response.set_entity(*handle);
  return response;
}

std::expected<players::BotResponse, std::string> Game::AddBot(const players::BotOptions& request) {
  // Resolve the hero and the bot service.
  auto heroes = services_.Heroes();
  if (!heroes) {
    return std::unexpected(heroes.error());
  }
  auto hero = FindHero(**heroes, request);
  if (!hero) {
    return std::unexpected(hero.error());
  }
  auto bots = services_.Bots();
  if (!bots) {
    return std::unexpected(bots.error());
  }

  // Create the bot and remember it, so Clear can kick it.
  auto slot = (*bots)->Create(request.name().c_str(), request.team(),
                              static_cast<uint32_t>(hero->id), Floats(request.position()));
  if (!slot) {
    return std::unexpected(slot.error());
  }
  bots_.emplace(*slot, request.name());
  players::BotResponse response;
  response.set_player(*slot);
  return response;
}

std::expected<void, std::string> Game::RemoveBot(const players::PlayerRequest& request) {
  // Find the bot the mod added in that slot.
  auto found = bots_.find(request.player());
  if (found == bots_.end()) {
    return std::unexpected("the mod added no bot in that slot");
  }

  // Kick the bot through the server console.
  auto server = services_.Server();
  if (!server) {
    return std::unexpected(server.error());
  }
  if (!(*server)->ServerCommand(("kick \"" + found->second + "\"\n").c_str())) {
    return std::unexpected("the server console is unavailable");
  }
  Depart(found->first);
  bots_.erase(found);
  return {};
}

std::expected<void, std::string> Game::Precache(const PrecacheOptions& request) {
  std::vector<std::string> resources;
  resources.reserve(request.resources().size());
  for (const auto& resource : request.resources()) {
    resources.push_back(WithAddon(resource));
  }
  return engine_->Precache({request.heroes().begin(), request.heroes().end()},
                           std::move(resources));
}

// The input calls replace the mod's changes; the shared hook applies them.

std::expected<void, std::string> Game::BlockInput(const input::InputRequest& request) {
  input_.Block(request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::BlockPlayerInput(const input::PlayerInputRequest& request) {
  input_.BlockPlayer(request.player(), request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::Press(const input::PlayerInputRequest& request) {
  input_.Press(request.player(), request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::WatchInput(const input::InputRequest& request) {
  input_.Watch(request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::RemapInput(const input::RemapInputRequest& request) {
  input_.Remap(request);
  services_.Input();
  return {};
}

gameinterop::WorldEntities::Prepare Game::WriteBeforeSpawn(
    const google::protobuf::RepeatedPtrField<fields::FieldWrite>& fields) {
  // The entity is not networked yet, so its fields need no change notice.
  return [this, &fields](void* created) -> std::expected<void, std::string> {
    for (const auto& write : fields) {
      auto stored =
          StoreField(created, write.class_name(), write.field(), write.type(), write.value());
      if (!stored) {
        return stored;
      }
    }
    return {};
  };
}

std::expected<entities::EntityResponse, std::string> Game::CreateEntity(
    const entities::EntityOptions& request) {
  // Resolve the world and the requested key values.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  std::deque<std::string> filled;
  auto key_values = KeyValues(request.key_values(), *this, filled);
  if (!key_values) {
    return std::unexpected(key_values.error());
  }

  // Describe the entity to create.
  const auto subclass =
      request.subclass().empty() ? 0 : gameinterop::WorldEntities::SubclassId(request.subclass());
  gameinterop::WorldEntities::Target target{
      .designer_name = request.designer_name(),
      .subclass_id = subclass,
      .team = request.team(),
      .position = Floats(request.position()),
      .facing = Floats(request.facing()),
      .velocity = {},
      .health = 0,
      .max_health = 0,
      .lane = std::nullopt,
  };

  // Create the entity, sent to every client when asked.
  auto entity = (*world)->CreateEntity(target, *key_values, WriteBeforeSpawn(request.fields()));
  if (!entity) {
    return std::unexpected(entity.error());
  }
  if (request.transmit_always()) {
    auto sent = (*world)->TransmitAlways(*entity);
    if (!sent || !*sent) {
      (void)(*world)->RemoveEntity(*entity);
      return std::unexpected(sent ? request.designer_name() + " did not survive its spawn"
                                  : sent.error());
    }
  }

  // Remember the entity as the mod's own and return it.
  created_.insert(*entity);
  entities::EntityResponse response;
  response.set_entity(*entity);
  return response;
}

std::expected<entities::AliveResponse, std::string> Game::FireInput(
    const entities::FireInputRequest& request) {
  // Fire the input on the entity, from its activator when given.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  std::optional<uint32_t> activator;
  if (request.has_activator()) {
    activator = request.activator();
  }
  return Alive(
      (*world)->FireInput(request.entity(), request.input(), Value(request.value()), activator));
}

std::expected<entities::AliveResponse, std::string> Game::RemoveEntity(
    const entities::EntityRequest& request) {
  // Resolve the world.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }

  // Only the mod's own entities are removed, so a stale or mistaken handle
  // cannot take a player or the map's entities out of the world.
  if (!created_.erase(request.entity())) {
    return std::unexpected("the mod did not create the entity");
  }
  visibility_.ShowEntityToAll(request.entity());
  return Alive((*world)->RemoveEntity(request.entity()));
}

std::expected<void, std::string> Game::ShowEntityOnlyTo(
    const entities::ShowEntityOnlyToRequest& request) {
  // Only the mod's own entities take an audience, so a mod cannot keep a
  // player's hero or the map's entities from a client.
  if (!created_.contains(request.entity())) {
    return std::unexpected("the mod did not create the entity");
  }
  const auto slots = Visibility::Audience(request.players());
  if (!slots) {
    return std::unexpected(slots.error());
  }

  // The engine sends only what its transmit pass allows, so an audience
  // needs the shared transmit hook.
  if (auto transmit = services_.Transmit(); !transmit) {
    return transmit;
  }
  visibility_.ShowEntityOnlyTo(request.entity(), *slots);
  return {};
}

std::expected<void, std::string> Game::ShowEntityToAll(const entities::EntityRequest& request) {
  if (!created_.contains(request.entity())) {
    return std::unexpected("the mod did not create the entity");
  }
  visibility_.ShowEntityToAll(request.entity());
  return {};
}

void Game::PruneAudiences() {
  if (!world_) {
    return;
  }
  // The transmit pass withholds by entity index alone, so a gone entity's
  // audience would otherwise apply to the next entity at its index.
  visibility_.Prune([this](uint32_t entity) {
    auto live = world_->Exists(entity);
    return live && !*live;
  });
}

std::optional<uint32_t> Game::ObjectHandle(uint32_t id) const {
  const auto found = objects_.find(id);
  if (found == objects_.end()) {
    return std::nullopt;
  }
  return std::visit([](const auto& object) { return object->Handle(); }, found->second);
}

std::expected<npcs::NpcResponse, std::string> Game::SpawnNpc(const npcs::NpcOptions& request) {
  // Resolve the world.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }

  // Describe the NPC, defaulting its maximum health to its health.
  const int32_t max_health = request.max_health() == 0 ? request.health() : request.max_health();
  gameinterop::WorldEntities::Target target{
      .designer_name = request.class_name(),
      .subclass_id =
          request.unit().empty() ? 0 : gameinterop::WorldEntities::SubclassId(request.unit()),
      .team = request.team(),
      .position = Floats(request.position()),
      .facing = Floats(request.facing()),
      .velocity = {},
      .health = request.health(),
      .max_health = max_health,
      .lane = request.has_lane() ? std::optional(request.lane()) : std::nullopt,
  };

  // Spawn the NPC and remember it as the mod's own.
  auto entity = (*world)->Spawn(target, WriteBeforeSpawn(request.fields()));
  if (!entity) {
    return std::unexpected(entity.error());
  }
  created_.insert(*entity);
  spawned_ = true;
  npcs::NpcResponse response;
  response.set_npc(*entity);
  return response;
}

std::expected<npcs::NpcStateResponse, std::string> Game::ReadNpc(const npcs::NpcRequest& request) {
  // Read the NPC's state.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  auto sample = (*world)->ReadNpc(request.npc());
  if (!sample) {
    return std::unexpected(sample.error());
  }

  // An NPC that is gone reads as an empty state.
  npcs::NpcStateResponse response;
  if (!*sample) {
    return response;
  }

  // Copy the NPC's state into the response.
  const auto& state = (*sample)->state;
  auto* npc = response.mutable_state();
  SetVector(npc->mutable_position(), state.position);
  SetAngles(npc->mutable_facing(), state.facing);
  npc->set_health(state.health);
  npc->set_max_health(state.max_health);
  npc->set_team(state.team);
  return response;
}

std::expected<entities::AliveResponse, std::string> Game::MoveNpc(
    const npcs::MoveNpcRequest& request) {
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  return Alive((*world)->Move(request.npc(), Floats(request.position()), Floats(request.facing()),
                              Floats(request.velocity())));
}

std::expected<entities::AliveResponse, std::string> Game::SetNpcHealth(
    const npcs::SetNpcHealthRequest& request) {
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  const int32_t max_health = request.has_max_health() ? request.max_health() : request.health();
  return Alive((*world)->SetHealth(request.npc(), request.health(), max_health));
}

std::expected<entities::AliveResponse, std::string> Game::RemoveNpc(
    const npcs::NpcRequest& request) {
  entities::EntityRequest entity;
  entity.set_entity(request.npc());
  return RemoveEntity(entity);
}

std::expected<entities::PickupResponse, std::string> Game::CreatePickup(
    const entities::CreatePickupRequest& request) {
  // Resolve the world.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }

  // A pickup reads its aura from its game data entry. The urn's entry shares
  // its designer name; a movement buff names the movement pickup entry.
  const bool urn = request.kind() == entities::PICKUP_KIND_URN;
  gameinterop::WorldEntities::Target target{
      .designer_name = urn ? "citadel_item_pickup_idol" : "citadel_item_pickup",
      .subclass_id = gameinterop::WorldEntities::SubclassId(urn ? "citadel_item_pickup_idol"
                                                                : "movement_powerup_pickup"),
      .team = 0,
      .position = Floats(request.position()),
      .facing = {},
      .velocity = {},
      .health = 0,
      .max_health = 0,
      .lane = std::nullopt,
  };

  // Create the pickup and remember it as the mod's own.
  auto entity = (*world)->CreateEntity(target, {});
  if (!entity) {
    return std::unexpected(entity.error());
  }
  created_.insert(*entity);
  entities::PickupResponse response;
  response.set_pickup(*entity);
  return response;
}

std::expected<entities::AliveResponse, std::string> Game::PickupPresent(
    const entities::PickupRequest& request) {
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  return Alive((*world)->Exists(request.pickup()));
}

std::expected<entities::AliveResponse, std::string> Game::RemovePickup(
    const entities::PickupRequest& request) {
  entities::EntityRequest entity;
  entity.set_entity(request.pickup());
  return RemoveEntity(entity);
}

std::expected<entities::EntitiesResponse, std::string> Game::FindEntities(
    const entities::FindEntitiesRequest& request) {
  // Find the entities of the class.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  auto found = (*world)->Find(request.class_name());
  if (!found) {
    return std::unexpected(found.error());
  }
  entities::EntitiesResponse response;
  response.mutable_entities()->Add(found->begin(), found->end());
  return response;
}

std::expected<entities::EntitiesResponse, std::string> Game::FindEntitiesByName(
    const entities::FindEntitiesByNameRequest& request) {
  // Find the entities with the name.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  auto found = (*world)->FindByName(request.name());
  if (!found) {
    return std::unexpected(found.error());
  }
  entities::EntitiesResponse response;
  response.mutable_entities()->Add(found->begin(), found->end());
  return response;
}

std::expected<entities::CountResponse, std::string> Game::RemoveEntities(
    const entities::RemoveEntitiesRequest& request) {
  // Remove the world's entities of the class and count them.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  auto removed = (*world)->Remove(request.class_name());
  if (!removed) {
    return std::unexpected(removed.error());
  }
  entities::CountResponse response;
  response.set_count(static_cast<int32_t>(*removed));
  return response;
}

std::expected<void, std::string> Game::ClearMap() {
  // Remove everything the mod authored in the world.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  auto damage = services_.Damage();
  if (!damage) {
    return std::unexpected(damage.error());
  }
  return (*world)->ClearAuthored(**damage);
}

}  // namespace modlock::wasm
