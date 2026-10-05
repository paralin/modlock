#include "wasm/game.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <span>
#include <utility>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/console_variables.h"
#include "modlock/gameinterop/keyvalues.h"
#include "modlock/gameinterop/native_memory.h"
#include "modlock/gameinterop/player_selection.h"
#include "proto/modlock/hud.pb.h"

namespace modlock::wasm {
namespace {

// kMaxPlayers bounds the player slots a Deadlock server holds.
constexpr int32_t kMaxPlayers = 64;

// kMaxString bounds the bytes ReadField copies for a string field.
constexpr size_t kMaxString = 256;

// kMaxChanges bounds the button changes, and separately the launches, one
// mod has not yet received.
constexpr size_t kMaxChanges = 256;

// kDesignerNameOffset is CEntityIdentity's designer name pointer.
constexpr size_t kDesignerNameOffset = 0x20;

std::array<float, 3> Floats(const Vec3& vector) {
  return {static_cast<float>(vector.x()), static_cast<float>(vector.y()),
          static_cast<float>(vector.z())};
}

std::array<float, 3> Floats(const EulerAngles& angles) {
  return {static_cast<float>(angles.pitch()), static_cast<float>(angles.yaw()),
          static_cast<float>(angles.roll())};
}

void SetAngles(EulerAngles* out, const std::array<float, 3>& angles) {
  out->set_pitch(angles[0]);
  out->set_yaw(angles[1]);
  out->set_roll(angles[2]);
}

void SetVector(Vec3* out, const std::array<float, 3>& vector) {
  out->set_x(vector[0]);
  out->set_y(vector[1]);
  out->set_z(vector[2]);
}

// DesignerName returns an entity's designer name, such as player, or an
// empty view.
std::string_view DesignerName(void* entity) {
  const auto* identity = static_cast<const unsigned char*>(gameinterop::IdentityOf(entity));
  if (identity == nullptr) return {};
  const char* name = nullptr;
  std::memcpy(&name, identity + kDesignerNameOffset, sizeof(name));
  return name == nullptr ? std::string_view() : std::string_view(name);
}

// Watches reports whether options name a projectile.
bool Watches(const ProjectileOptions& options, std::string_view name) {
  return !name.empty() && std::ranges::find(options.names(), name) != options.names().end();
}

// Size returns the bytes a field type occupies in the entity.
size_t Size(FieldType type) {
  switch (type) {
    case FIELD_TYPE_BOOL:
    case FIELD_TYPE_INT8:
    case FIELD_TYPE_UINT8:
      return 1;
    case FIELD_TYPE_INT16:
    case FIELD_TYPE_UINT16:
      return 2;
    case FIELD_TYPE_INT32:
    case FIELD_TYPE_UINT32:
    case FIELD_TYPE_FLOAT32:
    case FIELD_TYPE_HANDLE:
      return 4;
    case FIELD_TYPE_INT64:
    case FIELD_TYPE_UINT64:
    case FIELD_TYPE_FLOAT64:
    case FIELD_TYPE_STRING:
      return 8;
    case FIELD_TYPE_VECTOR:
      return 12;
    default:
      return 0;
  }
}

// Decode converts the bytes of one field to its value.
std::expected<FieldValue, std::string> Decode(FieldType type, const unsigned char* bytes) {
  // Copy a scalar of type T out of the field's bytes.
  auto as = [bytes]<typename T>(T) {
    T value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
  };
  FieldValue value;
  switch (type) {
    case FIELD_TYPE_BOOL:
      value.set_boolean(bytes[0] != 0);
      break;
    case FIELD_TYPE_INT8:
      value.set_number(as(int8_t{}));
      break;
    case FIELD_TYPE_INT16:
      value.set_number(as(int16_t{}));
      break;
    case FIELD_TYPE_INT32:
      value.set_number(as(int32_t{}));
      break;
    case FIELD_TYPE_UINT8:
      value.set_number(as(uint8_t{}));
      break;
    case FIELD_TYPE_UINT16:
      value.set_number(as(uint16_t{}));
      break;
    case FIELD_TYPE_UINT32:
      value.set_number(as(uint32_t{}));
      break;
    case FIELD_TYPE_INT64:
    case FIELD_TYPE_UINT64:
      // An unsigned value travels as the signed integer of the same bits.
      value.set_integer(as(int64_t{}));
      break;
    case FIELD_TYPE_FLOAT32:
      value.set_number(as(float{}));
      break;
    case FIELD_TYPE_FLOAT64:
      value.set_number(as(double{}));
      break;
    case FIELD_TYPE_VECTOR: {
      const auto vector = as(std::array<float, 3>{});
      value.mutable_vector()->set_x(vector[0]);
      value.mutable_vector()->set_y(vector[1]);
      value.mutable_vector()->set_z(vector[2]);
      break;
    }
    case FIELD_TYPE_HANDLE:
      value.set_number(as(uint32_t{}));
      break;
    case FIELD_TYPE_STRING: {
      // The field points to the characters; an empty pointer is empty text.
      const auto* text = as(static_cast<const char*>(nullptr));
      if (text == nullptr) {
        value.set_text("");
        break;
      }
      std::array<char, kMaxString> copy{};
      size_t length = 0;
      while (length < copy.size() && gameinterop::ReadNative(text + length, &copy[length], 1) &&
             copy[length] != '\0') {
        ++length;
      }
      value.set_text(std::string(copy.data(), length));
      break;
    }
    default:
      return std::unexpected("the field type is unknown");
  }
  return value;
}

// Encode converts a value to the bytes of one field of type. The value must
// take the form the type reads as. Text is not written, since its field
// points to memory the game owns.
std::expected<std::array<unsigned char, 12>, std::string> Encode(FieldType type,
                                                                 const FieldValue& value) {
  std::array<unsigned char, 12> bytes{};
  // Copy a scalar of type T into the field's bytes.
  auto put = [&bytes]<typename T>(T scalar) { std::memcpy(bytes.data(), &scalar, sizeof(scalar)); };
  auto expect = [&value](FieldValue::ValueCase form) -> std::expected<void, std::string> {
    if (value.value_case() != form) return std::unexpected("the value does not fit the field type");
    return {};
  };
  std::expected<void, std::string> fits;
  const double number = value.number();
  switch (type) {
    case FIELD_TYPE_BOOL:
      fits = expect(FieldValue::kBoolean);
      bytes[0] = value.boolean() ? 1 : 0;
      break;
    case FIELD_TYPE_INT8:
      fits = expect(FieldValue::kNumber);
      put(static_cast<int8_t>(number));
      break;
    case FIELD_TYPE_INT16:
      fits = expect(FieldValue::kNumber);
      put(static_cast<int16_t>(number));
      break;
    case FIELD_TYPE_INT32:
      fits = expect(FieldValue::kNumber);
      put(static_cast<int32_t>(number));
      break;
    case FIELD_TYPE_UINT8:
      fits = expect(FieldValue::kNumber);
      put(static_cast<uint8_t>(number));
      break;
    case FIELD_TYPE_UINT16:
      fits = expect(FieldValue::kNumber);
      put(static_cast<uint16_t>(number));
      break;
    case FIELD_TYPE_UINT32:
      fits = expect(FieldValue::kNumber);
      put(static_cast<uint32_t>(number));
      break;
    case FIELD_TYPE_INT64:
    case FIELD_TYPE_UINT64:
      fits = expect(FieldValue::kInteger);
      put(value.integer());
      break;
    case FIELD_TYPE_FLOAT32:
      fits = expect(FieldValue::kNumber);
      put(static_cast<float>(number));
      break;
    case FIELD_TYPE_FLOAT64:
      fits = expect(FieldValue::kNumber);
      put(number);
      break;
    case FIELD_TYPE_VECTOR:
      fits = expect(FieldValue::kVector);
      put(Floats(value.vector()));
      break;
    case FIELD_TYPE_HANDLE:
      fits = expect(FieldValue::kNumber);
      put(static_cast<uint32_t>(number));
      break;
    case FIELD_TYPE_STRING:
      return std::unexpected("text fields cannot be written");
    default:
      return std::unexpected("the field type is unknown");
  }
  if (!fits) return std::unexpected(fits.error());
  return bytes;
}

// FindHero resolves a request's hero by its identifier or its name.
template <typename Request>
std::expected<gameinterop::HeroDefinition, std::string> FindHero(
    const gameinterop::HeroDefinitions& heroes, const Request& request) {
  if (request.hero_case() == Request::kHeroId) return heroes.Find(request.hero_id());
  return heroes.Find(request.hero_name());
}

// Alive answers whether the entity a call acted on is still there.
std::expected<AliveResponse, std::string> Alive(std::expected<bool, std::string> alive) {
  if (!alive) return std::unexpected(alive.error());
  AliveResponse response;
  response.set_alive(*alive);
  return response;
}

// ScreenState returns the game's state for a screen effect.
std::expected<engine::ScreenEffectState, std::string> ScreenState(ScreenEffect effect) {
  switch (effect) {
    case SCREEN_EFFECT_KILLED:
      return engine::SCREEN_EFFECT_STATE_KILLED;
    case SCREEN_EFFECT_BLACK:
      return engine::SCREEN_EFFECT_STATE_BLACK;
    case SCREEN_EFFECT_BLINDED:
      return engine::SCREEN_EFFECT_STATE_BLINDED;
    case SCREEN_EFFECT_DARKNESS:
      return engine::SCREEN_EFFECT_STATE_DRIFTER_DARKNESS_CASTER;
    case SCREEN_EFFECT_MATCH_INTRO:
      return engine::SCREEN_EFFECT_STATE_MATCH_INTRO;
    default:
      return std::unexpected("the screen effect is unknown");
  }
}

// Remap moves the state of remap's from buttons to its to buttons.
void Remap(const RemapInputRequest& remap, gameinterop::AbilityInputHook::Input& input) {
  if (remap.from() == 0) return;
  const bool held = input.buttons[0] & remap.from();
  for (auto& state : input.buttons) {
    const bool set = state & remap.from();
    state &= ~remap.from();
    if (set) state |= remap.to();
  }
  if (held && remap.repeat()) input.buttons[1] |= remap.to();
}

}  // namespace

std::expected<const gameinterop::EngineServer*, std::string> GameServices::Server() {
  if (!server_) {
    auto server = gameinterop::EngineServer::Resolve();
    if (!server) return std::unexpected(server.error());
    server_ = *server;
  }
  return &*server_;
}

std::expected<const gameinterop::NativeUserMessages*, std::string> GameServices::Messages() {
  if (!messages_) {
    auto messages = gameinterop::NativeUserMessages::TryCreate();
    if (!messages) return std::unexpected(messages.error());
    messages_ = *messages;
  }
  return &*messages_;
}

std::expected<void, std::string> GameServices::ServerCommand(std::string command) {
  command += '\n';
  if (!started_) {
    pending_commands_.push_back(std::move(command));
    return {};
  }
  auto server = Server();
  if (!server) return std::unexpected(server.error());
  // A line that sets a variable may name a development-only or cheat-protected
  // one, which the console otherwise refuses. Exposing the first word of any
  // other line finds no variable and changes nothing.
  if (auto variables = gameinterop::ConsoleVariables::Resolve()) {
    const auto name = command.substr(0, command.find_first_of(" \t\n"));
    static_cast<void>(variables->AllowServerChanges(name.c_str()));
  }
  if (!(*server)->ServerCommand(command.c_str())) {
    return std::unexpected("the server console is unavailable");
  }
  return {};
}

void GameServices::WorldStarting() {
  started_ = true;
  auto pending = std::exchange(pending_commands_, {});
  for (auto& command : pending) {
    if (auto run = ServerCommand(std::move(command)); !run) {
      std::cerr << "server command: " << run.error() << '\n';
    }
  }
}

void GameServices::WorldReady() {
  // Mods loaded after the world started never saw it start.
  if (!started_) WorldStarting();
  world_ready_ = true;
  if (input_wanted_) Input();
  if (impacts_wanted_) Projectiles();
}

std::expected<const gameinterop::ModuleImage*, std::string> GameServices::Image() {
  if (!image_) {
    auto image = gameinterop::MappedModuleImage::ForModule(L"server.dll");
    if (!image) return std::unexpected(image.error());
    image_ = *image;
  }
  return &*image_;
}

std::expected<void*, std::string> GameServices::Schema() {
  return gameinterop::ResolveSchemaSystem();
}

std::expected<const gameinterop::HeroDefinitions*, std::string> GameServices::Heroes() {
  if (!heroes_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto heroes = gameinterop::HeroDefinitions::Resolve(**image);
    if (!heroes) return std::unexpected(heroes.error());
    heroes_ = *heroes;
  }
  return &*heroes_;
}

std::expected<const gameinterop::PlayerSelectionCalls*, std::string> GameServices::Selection() {
  if (!selection_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto selection = gameinterop::PlayerSelectionCalls::Resolve(**image);
    if (!selection) return std::unexpected(selection.error());
    selection_ = *selection;
  }
  return &*selection_;
}

std::expected<const gameinterop::AbilityDefinitions*, std::string> GameServices::Abilities() {
  if (!abilities_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto abilities = gameinterop::AbilityDefinitions::Resolve(**image);
    if (!abilities) return std::unexpected(abilities.error());
    abilities_ = *abilities;
  }
  return &*abilities_;
}

std::expected<const gameinterop::ItemFunctions*, std::string> GameServices::Items() {
  if (!items_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto items = gameinterop::ItemFunctions::Resolve(**image);
    if (!items) return std::unexpected(items.error());
    items_ = *items;
  }
  return &*items_;
}

std::expected<const gameinterop::AbilitySlots*, std::string> GameServices::Slots() {
  if (!slots_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto schema = Schema();
    if (!schema) return std::unexpected(schema.error());
    auto slots = gameinterop::AbilitySlots::Resolve(**image, *schema);
    if (!slots) return std::unexpected(slots.error());
    slots_ = *slots;
  }
  return &*slots_;
}

std::expected<gameinterop::CreateAbility, std::string> GameServices::CreateAbility() {
  if (!create_ability_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto create = gameinterop::ResolveCreateAbility(**image);
    if (!create) return std::unexpected(create.error());
    create_ability_ = *create;
  }
  return create_ability_;
}

std::expected<gameinterop::RespawnPawn, std::string> GameServices::Respawn() {
  if (!respawn_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto respawn = gameinterop::ResolveRespawnPawn(**image);
    if (!respawn) return std::unexpected(respawn.error());
    respawn_ = *respawn;
  }
  return respawn_;
}

std::expected<gameinterop::TeleportClientCamera, std::string> GameServices::Teleport() {
  if (!teleport_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto teleport = gameinterop::ResolveTeleportClientCamera(**image);
    if (!teleport) return std::unexpected(teleport.error());
    teleport_ = *teleport;
  }
  return teleport_;
}

std::expected<const gameinterop::BotCreation*, std::string> GameServices::Bots() {
  if (!bots_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto bots = gameinterop::BotCreation::Resolve(**image);
    if (!bots) return std::unexpected(bots.error());
    bots_ = *bots;
  }
  return &*bots_;
}

std::expected<render::WorldTextGameFactory*, std::string> GameServices::Text() {
  if (!text_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto text = render::WorldTextGameFactory::TryCreate(**image);
    if (!text) return std::unexpected(text.error());
    text_ = std::move(*text);
  }

  // Each world has its own entity system.
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) return std::unexpected(entities.error());
  text_->SetEntitySystem(*entities);
  return text_.get();
}

std::expected<render::WorldEffectGameFactory*, std::string> GameServices::Effects() {
  if (!effects_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto effects = render::WorldEffectGameFactory::TryCreate(**image);
    if (!effects) return std::unexpected(effects.error());
    effects_ = std::move(*effects);
  }
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) return std::unexpected(entities.error());
  effects_->SetEntitySystem(*entities);
  return effects_.get();
}

std::expected<const gameinterop::NativeDamage*, std::string> GameServices::Damage() {
  if (!damage_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto schema = Schema();
    if (!schema) return std::unexpected(schema.error());
    auto damage = gameinterop::NativeDamage::Resolve(**image, *schema);
    if (!damage) return std::unexpected(damage.error());
    damage_ = std::move(*damage);
  }
  return &*damage_;
}

std::expected<gameinterop::ModifyCurrency, std::string> GameServices::Currency() {
  if (!currency_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto currency = gameinterop::ResolveModifyCurrency(**image);
    if (!currency) return std::unexpected(currency.error());
    currency_ = *currency;
  }
  return currency_;
}

std::expected<const gameinterop::NativeSound*, std::string> GameServices::Sound() {
  if (!sound_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto sound = gameinterop::NativeSound::TryCreate(**image);
    if (!sound) return std::unexpected(sound.error());
    sound_ = std::move(*sound);
  }
  return &*sound_;
}

std::expected<const gameinterop::GamePause*, std::string> GameServices::Pause() {
  if (!pause_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto schema = Schema();
    if (!schema) return std::unexpected(schema.error());
    auto pause = gameinterop::GamePause::Resolve(**image, *schema);
    if (!pause) return std::unexpected(pause.error());
    pause_ = *pause;
  }
  return &*pause_;
}

std::expected<gameinterop::MatchClock*, std::string> GameServices::Clock() {
  if (!clock_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto schema = Schema();
    if (!schema) return std::unexpected(schema.error());
    auto clock = gameinterop::MatchClock::Resolve(**image, *schema);
    if (!clock) return std::unexpected(clock.error());
    clock_ = *clock;
  }
  return &*clock_;
}

std::expected<gameinterop::KothRules*, std::string> GameServices::Rift() {
  if (!rift_) {
    auto image = Image();
    if (!image) return std::unexpected(image.error());
    auto schema = Schema();
    if (!schema) return std::unexpected(schema.error());
    auto rift = gameinterop::KothRules::Resolve(**image, *schema);
    if (!rift) return std::unexpected(rift.error());
    rift_ = std::move(*rift);
  }
  return &*rift_;
}

std::expected<gameinterop::SchemaField, std::string> GameServices::Field(
    const std::string& class_name, const std::string& field) {
  FieldKey key{class_name, field};
  if (auto found = fields_.find(key); found != fields_.end()) return found->second;
  auto schema = Schema();
  if (!schema) return std::unexpected(schema.error());
  auto resolved =
      gameinterop::SchemaFieldOf(*schema, "server.dll", class_name.c_str(), field.c_str());
  if (!resolved) return std::unexpected(class_name + "." + field + ": " + resolved.error());
  fields_.emplace(std::move(key), *resolved);
  return *resolved;
}

std::expected<GameServices::ModifierLayout, std::string> GameServices::Modifiers() {
  if (modifiers_) return *modifiers_;
  auto property = Field("CBaseEntity", "m_pModifierProp");
  if (!property) return std::unexpected(property.error());
  auto mask = Field("CModifierProperty", "m_bvEnabledStateMask");
  if (!mask) return std::unexpected(mask.error());
  auto predicted = Field("CModifierProperty", "m_bvEnabledPredictedStateMask");
  if (!predicted) return std::unexpected(predicted.error());
  modifiers_ = ModifierLayout{
      .property = property->offset,
      .mask = mask->offset,
      .predicted_mask = predicted->offset,
      .mask_size = std::min(mask->size, predicted->size),
  };
  return *modifiers_;
}

std::expected<int64_t, std::string> GameServices::ModifierState(const std::string& name) {
  if (auto found = modifier_states_.find(name); found != modifier_states_.end()) {
    return found->second;
  }
  auto schema = Schema();
  if (!schema) return std::unexpected(schema.error());
  auto value =
      gameinterop::SchemaEnumValueOf(*schema, "server.dll", "EModifierState", name.c_str());
  if (!value) return std::unexpected(name + ": " + value.error());
  modifier_states_.emplace(name, *value);
  return *value;
}

void GameServices::Input() {
  input_wanted_ = true;
  if (input_ || !world_ready_) return;
  auto image = Image();
  auto schema = Schema();
  if (!image || !schema) return;

  // Bots take input too, so a mod can press their buttons.
  auto input = gameinterop::AbilityInputHook::Install(
      **image, *schema,
      [this](gameinterop::AbilityInputHook::Input& input) { return Filter(input); },
      [](std::string error) { std::cerr << "mod input: " << error << '\n'; }, true);
  if (!input) {
    std::cerr << "mod input: " << input.error() << '\n';
    return;
  }
  input_ = std::move(*input);
}

uint64_t GameServices::Filter(gameinterop::AbilityInputHook::Input& input) {
  // A press is a button held now that changed for this input; a release is
  // one that changed and is up.
  const uint64_t pressed = input.buttons[0] & input.buttons[1];
  const uint64_t released = ~input.buttons[0] & input.buttons[1];
  uint64_t blocked = 0;
  for (auto* game : games_) {
    blocked |= game->blocked_;
    if (auto found = game->slot_blocked_.find(input.slot); found != game->slot_blocked_.end()) {
      blocked |= found->second;
    }
    if ((pressed | released) & game->watched_ && game->changes_.size() < kMaxChanges) {
      game->changes_.push_back({input.slot, pressed & game->watched_, released & game->watched_});
    }

    auto press = game->presses_.find(input.slot);
    if (press == game->presses_.end()) continue;

    // A press is down and newly changed for this one input.
    input.buttons[0] |= press->second;
    input.buttons[1] |= press->second;
    input.buttons[2] &= ~press->second;
    game->presses_.erase(press);
  }
  for (auto* game : games_) Remap(game->remap_, input);
  return blocked;
}

void GameServices::Projectiles() {
  impacts_wanted_ = true;
  if (impacts_ || !world_ready_) return;
  auto image = Image();
  if (!image) return;
  auto hook = gameinterop::ProjectileImpactHook::Install(
      **image, [this](void* entity, const auto& contact, const auto& native) {
        Impact(entity, contact, native);
      });
  if (!hook) {
    std::cerr << "mod projectiles: " << hook.error() << '\n';
    return;
  }
  impacts_ = std::move(*hook);
}

void GameServices::Impact(void* entity, const gameinterop::TraceResult& contact,
                          const gameinterop::ProjectileImpactHook::NativeImpact& native) {
  std::vector<std::pair<Game*, ImpactEvent>> watching;
  bool keep = false;
  for (auto* game : games_) {
    auto event = game->Watching(entity, contact);
    if (!event) continue;
    keep |= game->projectiles_.keep_momentum();
    watching.emplace_back(game, std::move(*event));
  }
  if (watching.empty()) {
    native();
    return;
  }

  // The game's explosion replaces the motion of heroes near it, even with no
  // push of its own.
  std::vector<Game::Hero> heroes;
  if (keep) heroes = watching.front().first->Heroes();
  native();
  for (const auto& hero : heroes) {
    if (hero.velocity) gameinterop::SetEntityVelocity(hero.pawn, *hero.velocity);
  }
  for (auto& [game, event] : watching) {
    if (games_.contains(game)) game->Impacted(event);
  }
}

Game::Game(GameServices& services, EngineHost* engine)
    : services_(services),
      engine_(engine),
      observer_(gameinterop::PawnObserver::LiveSeams(true), true) {
  services_.games_.insert(this);
}

Game::~Game() { services_.games_.erase(this); }

void Game::Frame() {
  ApplyHolds();
  HoldClock();
  TickRestores();
  TrackProjectiles();
  if (!spawned_ || !world_) return;
  world_->FinishSpawns();
  spawned_ = false;
}

void Game::ApplyHolds() {
  std::erase_if(
      held_, [this](const auto& hold) { return !SetModifierState(hold.first, hold.second, true); });
  for (const auto& [slot, key] : held_modifiers_) static_cast<void>(RenewModifier(slot, key));
}

std::expected<void, std::string> Game::RenewModifier(int32_t slot, const std::string& key) {
  const auto modifier = modifiers_.find(key);
  if (modifier == modifiers_.end()) return std::unexpected("the modifier is not held");
  const auto sample = observer_.Observe(slot);
  if (!sample || sample->health <= 0) return {};
  auto attached = modifier->second.Attached(observer_, slot);
  if (!attached) return std::unexpected(attached.error());
  if (*attached) return {};
  const auto ability = std::string_view(key).substr(0, key.find('/'));
  return modifier->second.Attach(observer_, slot, gameinterop::MakeMemberName(ability).hash);
}

void Game::ReleaseModifier(int32_t slot, const std::string& key) {
  if (held_modifiers_.erase({slot, key}) == 0) return;
  if (std::ranges::any_of(held_modifiers_, [&](const auto& hold) { return hold.second == key; }))
    return;
  if (auto modifier = modifiers_.find(key); modifier != modifiers_.end()) {
    modifier->second.Restore();
    modifiers_.erase(modifier);
  }
}

void Game::ReleaseModifiers() {
  for (const auto& [key, modifier] : modifiers_) modifier.Restore();
  modifiers_.clear();
  held_modifiers_.clear();
}

std::vector<Game::ButtonChange> Game::TakeButtonChanges() { return std::exchange(changes_, {}); }

std::vector<Game::Restored> Game::TakeRestored() { return std::exchange(restored_, {}); }

std::optional<std::string> Game::TakeNpcsRestored() { return std::exchange(npcs_restored_, {}); }

void Game::WorldEnding() {
  RemoveWorld();
  world_.reset();
  spawned_ = false;
  frozen_.clear();
  held_.clear();
  ReleaseModifiers();
  presses_.clear();
  restores_.clear();
  npc_restore_.reset();
  held_clock_.reset();
  paused_ = false;
  observer_.Invalidate();
}

void Game::Leave(int32_t slot) {
  frozen_.erase(slot);
  movers_.erase(slot);
  restores_.erase(slot);
  slot_blocked_.erase(slot);
  presses_.erase(slot);
  std::vector<std::string> keys;
  for (const auto& [held, key] : held_modifiers_) {
    if (held == slot) keys.push_back(key);
  }
  for (const auto& key : keys) ReleaseModifier(slot, key);
}

void Game::Clear() {
  RemoveWorld();
  for (const auto slot : frozen_) {
    if (observer_.Observe(slot)) static_cast<void>(observer_.SetPreparationFrozen(slot, false));
  }
  frozen_.clear();
  movers_.clear();
  for (const auto& [entity, state] : held_) {
    static_cast<void>(SetModifierState(entity, state, false));
  }
  held_.clear();
  ReleaseModifiers();
  blocked_ = 0;
  slot_blocked_.clear();
  remap_.Clear();
  presses_.clear();
  watched_ = 0;
  changes_.clear();
  restores_.clear();
  restored_.clear();
  npc_restore_.reset();
  npcs_restored_.reset();
  held_clock_.reset();
  if (paused_) static_cast<void>(SetPaused(false));
  projectiles_.Clear();
}

void Game::RemoveWorld() {
  for (auto& [id, object] : objects_) {
    std::visit([](auto& handle) { handle->Remove(); }, object);
  }
  objects_.clear();
  if (fog_) {
    fog_->Remove();
    fog_.reset();
  }
  if (world_) {
    for (const auto entity : npcs_) static_cast<void>(world_->RemoveNpc(entity));
  }
  npcs_.clear();

  // Only the engine's kick removes a bot's connection.
  if (auto server = services_.Server()) {
    for (const auto& [slot, name] : bots_) {
      static_cast<void>((*server)->ServerCommand(("kick \"" + name + "\"\n").c_str()));
    }
  }
  bots_.clear();
}

std::expected<void, std::string> Game::ServerCommand(const ServerCommandRequest& request) {
  return services_.ServerCommand(request.command());
}

std::expected<void, std::string> Game::Chat(const ChatRequest& request) {
  auto messages = services_.Messages();
  if (!messages) return std::unexpected(messages.error());
  return (*messages)->Chat(request.player(), request.text());
}

std::expected<void, std::string> Game::CenterText(const CenterTextRequest& request) {
  auto messages = services_.Messages();
  if (!messages) return std::unexpected(messages.error());
  return (*messages)->CenterText(request.player(), request.text());
}

std::expected<void, std::string> Game::Announce(const AnnounceRequest& request) {
  auto messages = services_.Messages();
  if (!messages) return std::unexpected(messages.error());
  return (*messages)->Announce(request.player(), request.title(), request.text());
}

std::expected<PlayersResponse, std::string> Game::Players() {
  PlayersResponse players;
  for (int32_t slot = 0; slot < kMaxPlayers; ++slot) {
    const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
    if (!state.occupied) continue;
    auto* player = players.add_players();
    player->set_player(slot);
    player->set_steam_id(state.is_bot ? 0 : state.xuid);
    player->set_name(state.name);
    player->set_bot(state.is_bot);
    player->set_ready(state.is_bot || state.fully_connected);
    player->set_generation(state.generation);
  }
  return players;
}

std::expected<PawnResponse, std::string> Game::Pawn(const PlayerRequest& request) {
  PawnResponse response;
  const auto sample = observer_.Observe(request.player());
  if (!sample) return response;
  auto* pawn = response.mutable_pawn();
  pawn->set_entity(sample->pawn_handle);
  pawn->set_hero(sample->hero_id);
  pawn->set_team(sample->team);
  pawn->set_health(sample->health);
  pawn->set_max_health(sample->max_health);
  pawn->mutable_position()->set_x(sample->x);
  pawn->mutable_position()->set_y(sample->y);
  pawn->mutable_position()->set_z(sample->z);
  SetAngles(pawn->mutable_eye_angles(), sample->eye_angles);
  SetAngles(pawn->mutable_camera_angles(), sample->camera_angles);
  if (sample->stamina) {
    pawn->set_stamina(sample->stamina->current);
    pawn->set_max_stamina(sample->stamina->max);
  }
  pawn->set_generation(sample->session_generation);
  if (sample->currencies) pawn->set_souls((*sample->currencies)[0]);
  if (sample->eye_position) SetVector(pawn->mutable_eye_position(), *sample->eye_position);
  if (sample->movement && sample->movement->abs_velocity)
    SetVector(pawn->mutable_velocity(), *sample->movement->abs_velocity);
  return response;
}

std::expected<std::pair<void*, unsigned char*>, std::string> Game::FieldAddress(
    uint32_t handle, const std::string& class_name, const std::string& field, FieldType type) {
  const auto size = Size(type);
  if (size == 0) return std::unexpected("the field type is unknown");
  auto found = services_.Field(class_name, field);
  if (!found) return std::unexpected(found.error());
  if (found->size < size) {
    return std::unexpected(class_name + "." + field + " is smaller than its requested type");
  }
  auto entity = Entity(handle);
  if (!entity) return std::unexpected(entity.error());
  return std::pair{*entity, static_cast<unsigned char*>(*entity) + found->offset};
}

std::expected<FieldResponse, std::string> Game::ReadField(const ReadFieldRequest& request) {
  auto address =
      FieldAddress(request.entity(), request.class_name(), request.field(), request.type());
  if (!address) return std::unexpected(address.error());

  // Copy through the operating system, so a field the entity lacks fails
  // instead of faulting the server.
  std::array<unsigned char, 12> bytes{};
  if (!gameinterop::ReadNative(address->second, bytes.data(), Size(request.type()))) {
    return std::unexpected("cannot read " + request.class_name() + "." + request.field());
  }
  auto value = Decode(request.type(), bytes.data());
  if (!value) return std::unexpected(value.error());
  FieldResponse response;
  *response.mutable_value() = std::move(*value);
  return response;
}

std::expected<void, std::string> Game::WriteField(const WriteFieldRequest& request) {
  auto bytes = Encode(request.type(), request.value());
  if (!bytes) return std::unexpected(bytes.error());
  auto address =
      FieldAddress(request.entity(), request.class_name(), request.field(), request.type());
  if (!address) return std::unexpected(address.error());
  if (!gameinterop::WriteNative(address->second, bytes->data(), Size(request.type()))) {
    return std::unexpected("cannot write " + request.class_name() + "." + request.field());
  }
  if (!gameinterop::NotifyEntityStateChanged(address->first)) {
    return std::unexpected("cannot replicate " + request.class_name() + "." + request.field());
  }
  return {};
}

std::expected<ActiveResponse, std::string> Game::ModifierState(
    const ModifierStateRequest& request) {
  auto entity = Entity(request.entity());
  if (!entity) return std::unexpected(entity.error());
  auto word = FindModifierWord(*entity, request.state());
  if (!word) return std::unexpected(word.error());
  ActiveResponse response;
  if (!*word) return response;

  // The game predicts some states, such as parry, so a state is active when
  // either mask holds it.
  uint32_t ordinary = 0;
  uint32_t predicted = 0;
  if (!gameinterop::ReadNative((*word)->ordinary, &ordinary, sizeof(ordinary)) ||
      !gameinterop::ReadNative((*word)->predicted, &predicted, sizeof(predicted))) {
    return std::unexpected("cannot read the entity's modifier states");
  }
  response.set_active(((ordinary | predicted) & (*word)->bit) != 0);
  return response;
}

std::expected<void, std::string> Game::HoldModifierState(const HoldModifierStateRequest& request) {
  auto hold = std::pair(request.entity(), request.state());
  if (request.has_active() && !request.active()) {
    if (held_.erase(hold) == 0) return {};
    return SetModifierState(request.entity(), request.state(), false);
  }
  if (auto set = SetModifierState(request.entity(), request.state(), true); !set) return set;
  held_.insert(std::move(hold));
  return {};
}

std::expected<std::optional<Game::ModifierWord>, std::string> Game::FindModifierWord(
    void* entity, const std::string& state) {
  auto layout = services_.Modifiers();
  if (!layout) return std::unexpected(layout.error());
  auto value = services_.ModifierState(state);
  if (!value) return std::unexpected(value.error());
  if (*value < 0 || static_cast<size_t>(*value) >= layout->mask_size * 8) {
    return std::unexpected(state + " is outside the modifier state masks");
  }
  unsigned char* property = nullptr;
  if (!gameinterop::ReadNative(static_cast<const unsigned char*>(entity) + layout->property,
                               &property, sizeof(property))) {
    return std::unexpected("cannot read the entity's modifiers");
  }
  if (property == nullptr) return std::nullopt;
  const auto word = static_cast<size_t>(*value / 32) * sizeof(uint32_t);
  return ModifierWord{
      .ordinary = property + layout->mask + word,
      .predicted = property + layout->predicted_mask + word,
      .bit = uint32_t{1} << (*value % 32),
  };
}

std::expected<void, std::string> Game::SetModifierState(uint32_t handle, const std::string& state,
                                                        bool active) {
  auto entity = Entity(handle);
  if (!entity) return std::unexpected(entity.error());
  auto word = FindModifierWord(*entity, state);
  if (!word) return std::unexpected(word.error());
  if (!*word) return std::unexpected("the entity has no modifiers");
  uint32_t current = 0;
  if (!gameinterop::ReadNative((*word)->ordinary, &current, sizeof(current))) {
    return std::unexpected("cannot read the entity's modifier states");
  }
  const uint32_t desired = active ? current | (*word)->bit : current & ~(*word)->bit;
  if (desired == current) return {};
  if (!gameinterop::WriteNative((*word)->ordinary, &desired, sizeof(desired))) {
    return std::unexpected("cannot write the entity's modifier states");
  }
  if (!gameinterop::NotifyEntityStateChanged(*entity)) {
    return std::unexpected("cannot replicate the entity's modifier states");
  }
  return {};
}

std::expected<HeroResponse, std::string> Game::SelectHero(const SelectHeroRequest& request) {
  auto heroes = services_.Heroes();
  if (!heroes) return std::unexpected(heroes.error());
  auto hero = FindHero(**heroes, request);
  if (!hero) return std::unexpected(hero.error());
  auto selection = services_.Selection();
  if (!selection) return std::unexpected(selection.error());
  const auto state = gameinterop::ConnectionTracker::StateForSlot(request.player());
  if (!state.occupied) return std::unexpected("no player is in that slot");
  auto selected =
      observer_.SelectPlayer(request.player(), state.xuid, state.generation, request.team(),
                             hero->native_definition_pointer, **selection);
  if (!selected) return std::unexpected(selected.error());
  HeroResponse response;
  response.set_hero(static_cast<uint32_t>(hero->id));
  return response;
}

std::expected<void, std::string> Game::Respawn(const PlayerRequest& request) {
  auto respawn = services_.Respawn();
  if (!respawn) return std::unexpected(respawn.error());
  auto sample = Live(request.player());
  if (!sample) return std::unexpected(sample.error());
  return observer_.RespawnPlayer(request.player(), sample->session_generation, *respawn);
}

std::expected<void, std::string> Game::ClearItems(const PlayerRequest& request) {
  auto abilities = services_.Abilities();
  if (!abilities) return std::unexpected(abilities.error());
  auto items = services_.Items();
  if (!items) return std::unexpected(items.error());
  if (auto sample = Live(request.player()); !sample) return std::unexpected(sample.error());
  return observer_.ReconcileItems(request.player(), {}, **abilities, **items);
}

std::expected<void, std::string> Game::Freeze(const FreezeRequest& request) {
  const auto slot = request.player();
  const bool frozen = !request.has_frozen() || request.frozen();
  if (auto sample = Live(slot); !sample) return std::unexpected(sample.error());
  if (auto done = observer_.SetPreparationFrozen(slot, frozen); !done) return done;
  if (frozen) {
    frozen_.insert(slot);
  } else {
    frozen_.erase(slot);
  }
  return {};
}

std::expected<void, std::string> Game::RestoreStamina(const PlayerRequest& request) {
  auto server = services_.Server();
  if (!server) return std::unexpected(server.error());
  auto clock = (*server)->ReadClock();
  if (!clock) return std::unexpected(clock.error());
  if (auto sample = Live(request.player()); !sample) return std::unexpected(sample.error());
  return observer_.RestorePracticeStamina(request.player(), clock->current_time);
}

std::expected<void, std::string> Game::RefreshAbility(const RefreshAbilityRequest& request) {
  auto ability = OwnedAbility(request.player(), request.ability());
  if (!ability) return std::unexpected(ability.error());
  ability->cooldown_start = 0;
  ability->cooldown_end = 0;
  return observer_.ApplyAbilityTimers(request.player(), std::span(&*ability, 1));
}

std::expected<AbilitiesResponse, std::string> Game::Abilities(const PlayerRequest& request) {
  const auto slot = request.player();
  auto definitions = services_.Abilities();
  if (!definitions) return std::unexpected(definitions.error());
  if (auto sample = Live(slot); !sample) return std::unexpected(sample.error());
  auto abilities = observer_.CurrentAbilitiesForSlot(slot);
  if (!abilities) return std::unexpected(abilities.error());
  AbilitiesResponse response;
  for (const auto& ability : *abilities) {
    auto* out = response.add_abilities();
    if (auto definition = (*definitions)->Find(ability.subclass_id)) {
      out->set_name(definition->name);
    }
    out->set_slot(ability.slot);
    out->set_entity(ability.handle);
    out->set_upgrades(ability.upgrade_info >> 16);
    out->set_charges(ability.charges);
    out->set_cooldown_end(ability.cooldown_end);
    out->set_id(ability.subclass_id);
    out->set_state(ability.upgrade_info);
    out->set_cooldown_start(ability.cooldown_start);
    out->set_recharge_start(ability.charge_recharge_start);
    out->set_recharge_end(ability.charge_recharge_end);
  }
  return response;
}

std::expected<void, std::string> Game::SetAbility(const AbilityOptions& request) {
  auto ability = OwnedAbility(request.player(), request.ability());
  if (!ability) return std::unexpected(ability.error());
  if (request.has_upgrades()) {
    auto items = services_.Items();
    if (!items) return std::unexpected(items.error());
    const gameinterop::AbilityUpgrade upgrade{
        .subclass_id = ability->subclass_id,
        .slot = ability->slot,
        .upgrade_info = (ability->upgrade_info & 0xffff) | (request.upgrades() << 16),
    };
    if (auto applied = observer_.ApplyAbilityUpgrades(request.player(), std::span(&upgrade, 1),
                                                      (*items)->set_bits);
        !applied) {
      return applied;
    }
  }
  if (!request.has_charges() && !request.has_cooldown_end() && !request.has_recharge_end())
    return {};

  // An upgrade can change the charges, so set them on the upgraded ability.
  ability = OwnedAbility(request.player(), request.ability());
  if (!ability) return std::unexpected(ability.error());
  if (request.has_charges()) ability->charges = request.charges();
  if (request.has_cooldown_end()) ability->cooldown_end = request.cooldown_end();
  if (request.has_recharge_end()) ability->charge_recharge_end = request.recharge_end();
  return observer_.ApplyAbilityTimers(request.player(), std::span(&*ability, 1));
}

std::expected<void, std::string> Game::GiveItem(const GiveItemRequest& request) {
  auto definitions = services_.Abilities();
  if (!definitions) return std::unexpected(definitions.error());
  auto items = services_.Items();
  if (!items) return std::unexpected(items.error());
  if (auto sample = Live(request.player()); !sample) return std::unexpected(sample.error());
  auto item = observer_.GrantItem(
      request.player(), gameinterop::MakeMemberName(request.item()).hash, **definitions, **items);
  if (!item) return std::unexpected(item.error());
  return {};
}

std::expected<void, std::string> Game::ReplaceAbility(const ReplaceAbilityRequest& request) {
  if (request.index() > UINT16_MAX) return std::unexpected("the ability slot is out of range");
  auto definitions = services_.Abilities();
  if (!definitions) return std::unexpected(definitions.error());
  auto slots = services_.Slots();
  if (!slots) return std::unexpected(slots.error());
  auto create = services_.CreateAbility();
  if (!create) return std::unexpected(create.error());
  if (auto sample = Live(request.player()); !sample) return std::unexpected(sample.error());
  return (*slots)->Replace(observer_, request.player(), static_cast<uint16_t>(request.index()),
                           gameinterop::MakeMemberName(request.ability()).hash, **definitions,
                           *create);
}

std::expected<void, std::string> Game::HoldModifier(const HoldModifierRequest& request) {
  const auto& key = request.modifier();
  if (request.has_active() && !request.active()) {
    ReleaseModifier(request.player(), key);
    return {};
  }
  if (key.find('/') == std::string::npos)
    return std::unexpected("name the modifier as <ability>/<modifier>");
  if (!modifiers_.contains(key)) {
    auto image = services_.Image();
    if (!image) return std::unexpected(image.error());
    auto schema = services_.Schema();
    if (!schema) return std::unexpected(schema.error());
    auto modifier = gameinterop::AbilityModifier::Resolve(**image, *schema, key, -1);
    if (!modifier) return std::unexpected(modifier.error());
    modifiers_.emplace(key, *modifier);
  }
  held_modifiers_.emplace(request.player(), key);

  // A hold that cannot start now ends, so the mod learns why.
  auto renewed = RenewModifier(request.player(), key);
  if (!renewed) ReleaseModifier(request.player(), key);
  return renewed;
}

std::expected<void, std::string> Game::GiveModifier(const GiveModifierRequest& request) {
  const auto& key = request.modifier();
  const auto slash = key.find('/');
  if (slash == std::string::npos)
    return std::unexpected("name the modifier as <ability>/<modifier>");
  auto image = services_.Image();
  if (!image) return std::unexpected(image.error());
  auto schema = services_.Schema();
  if (!schema) return std::unexpected(schema.error());
  if (auto sample = Live(request.player()); !sample) return std::unexpected(sample.error());
  auto owned = observer_.CurrentAbilitiesForSlot(request.player());
  if (!owned) return std::unexpected(owned.error());

  // The modifier's own ability applies it when the hero owns that ability;
  // otherwise the hero's first ability does.
  auto source = gameinterop::MakeMemberName(std::string_view(key).substr(0, slash)).hash;
  if (std::ranges::none_of(*owned,
                           [&](const auto& ability) { return ability.subclass_id == source; })) {
    const auto first = std::ranges::find(*owned, 0, &gameinterop::PawnObserver::Ability::slot);
    if (first == owned->end())
      return std::unexpected("the hero has no ability to apply the modifier");
    source = first->subclass_id;
  }

  // The modifier takes its duration when attached, so the shared definition
  // gets its own duration back right away.
  auto modifier = gameinterop::AbilityModifier::Resolve(**image, *schema, key, request.seconds());
  if (!modifier) return std::unexpected(modifier.error());
  auto attached = modifier->Attach(observer_, request.player(), source);
  modifier->Restore();
  return attached;
}

std::expected<gameinterop::PawnObserver::Ability, std::string> Game::OwnedAbility(
    int32_t slot, std::string_view name) {
  auto definitions = services_.Abilities();
  if (!definitions) return std::unexpected(definitions.error());
  if (auto sample = Live(slot); !sample) return std::unexpected(sample.error());
  auto abilities = observer_.CurrentAbilitiesForSlot(slot);
  if (!abilities) return std::unexpected(abilities.error());
  for (const auto& ability : *abilities) {
    const auto definition = (*definitions)->Find(ability.subclass_id);
    if (definition && definition->name == name) return ability;
  }
  return std::unexpected("the hero has no ability named " + std::string(name));
}

std::expected<void, std::string> Game::Teleport(const TeleportRequest& request) {
  auto teleport = services_.Teleport();
  if (!teleport) return std::unexpected(teleport.error());
  if (auto sample = Live(request.player()); !sample) return std::unexpected(sample.error());
  const auto position = Floats(request.position());
  const auto angles = Floats(request.facing());
  auto* pawn = observer_.PawnForSlot(request.player());
  if (request.has_velocity()) {
    gameinterop::TeleportEntity(pawn, position, angles, Floats(request.velocity()));
  } else {
    (*teleport)(nullptr, pawn, position.data(), angles.data());
  }

  // A teleport recomputes the hero's modifier states, dropping held ones.
  ApplyHolds();
  return {};
}

std::expected<ObjectResponse, std::string> Game::CreateModel(const ModelOptions& request) {
  auto effects = services_.Effects();
  if (!effects) return std::unexpected(effects.error());
  render::WorldModelSettings settings{
      .resource = request.resource(),
      .origin = request.position(),
      .angles = Floats(request.facing()),
      .scale = request.scale() == 0 ? 1 : request.scale(),
      // The factory reads red from the low byte, as 0xAABBGGRR.
      .color_rgba = request.color() == 0 ? UINT32_MAX : std::byteswap(request.color()),
      .glow = request.glow(),
  };
  auto model = (*effects)->CreateModel(settings);
  if (!model) return std::unexpected(model.error());
  return Keep(std::move(*model));
}

std::expected<ObjectResponse, std::string> Game::CreateText(const TextOptions& request) {
  auto text = services_.Text();
  if (!text) return std::unexpected(text.error());
  render::WorldTextStyle style{
      .font_size = request.font_size() == 0 ? 100 : request.font_size(),
      // The entity takes its color as 0xAABBGGRR.
      .color_abgr = request.color() == 0 ? UINT32_MAX : std::byteswap(request.color()),
      .face_camera = request.face_camera(),
      .scale = request.scale() == 0 ? 1 : request.scale(),
  };
  auto entity = (*text)->Create(request.text(), request.position(), request.facing(), style);
  if (!entity) return std::unexpected(entity.error());
  return Keep(std::move(*entity));
}

std::expected<void, std::string> Game::MoveObject(const MoveObjectRequest& request) {
  auto found = objects_.find(request.object());
  if (found == objects_.end()) return std::unexpected("no object has that identifier");
  if (auto* model = std::get_if<std::unique_ptr<render::WorldEffect>>(&found->second)) {
    (*model)->Move(request.position());
  } else {
    std::get<std::unique_ptr<render::WorldTextEntity>>(found->second)
        ->SetOrigin(request.position(), request.facing());
  }
  return {};
}

std::expected<void, std::string> Game::SetText(const SetTextRequest& request) {
  auto found = objects_.find(request.object());
  if (found == objects_.end()) return std::unexpected("no object has that identifier");
  auto* text = std::get_if<std::unique_ptr<render::WorldTextEntity>>(&found->second);
  if (text == nullptr) return std::unexpected("the object is not text");
  (*text)->SetMessage(request.text());
  return {};
}

std::expected<void, std::string> Game::RemoveObject(const ObjectRequest& request) {
  auto found = objects_.find(request.object());
  if (found == objects_.end()) return std::unexpected("no object has that identifier");
  std::visit([](auto& handle) { handle->Remove(); }, found->second);
  objects_.erase(found);
  return {};
}

std::expected<BotResponse, std::string> Game::AddBot(const BotOptions& request) {
  auto heroes = services_.Heroes();
  if (!heroes) return std::unexpected(heroes.error());
  auto hero = FindHero(**heroes, request);
  if (!hero) return std::unexpected(hero.error());
  auto bots = services_.Bots();
  if (!bots) return std::unexpected(bots.error());
  auto slot = (*bots)->Create(request.name().c_str(), request.team(),
                              static_cast<uint32_t>(hero->id), Floats(request.position()));
  if (!slot) return std::unexpected(slot.error());
  bots_.emplace(*slot, request.name());
  BotResponse response;
  response.set_player(*slot);
  return response;
}

std::expected<void, std::string> Game::RemoveBot(const PlayerRequest& request) {
  auto found = bots_.find(request.player());
  if (found == bots_.end()) return std::unexpected("the mod added no bot in that slot");
  auto server = services_.Server();
  if (!server) return std::unexpected(server.error());
  if (!(*server)->ServerCommand(("kick \"" + found->second + "\"\n").c_str())) {
    return std::unexpected("the server console is unavailable");
  }
  bots_.erase(found);
  return {};
}

std::expected<void, std::string> Game::Precache(const PrecacheOptions& request) {
  return engine_->Precache({request.heroes().begin(), request.heroes().end()},
                           {request.resources().begin(), request.resources().end()});
}

// The input calls replace the mod's changes; the shared hook applies them.

std::expected<void, std::string> Game::BlockInput(const InputRequest& request) {
  blocked_ = request.buttons();
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::BlockPlayerInput(const PlayerInputRequest& request) {
  if (request.buttons() == 0) {
    slot_blocked_.erase(request.player());
  } else {
    slot_blocked_[request.player()] = request.buttons();
  }
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::Press(const PlayerInputRequest& request) {
  presses_[request.player()] |= request.buttons();
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::WatchInput(const InputRequest& request) {
  watched_ = request.buttons();
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::RemapInput(const RemapInputRequest& request) {
  remap_ = request;
  services_.Input();
  return {};
}

std::expected<NpcResponse, std::string> Game::SpawnNpc(const NpcOptions& request) {
  auto world = World();
  if (!world) return std::unexpected(world.error());
  const int32_t max_health = request.max_health() == 0 ? request.health() : request.max_health();
  gameinterop::WorldEntities::Target target{
      .designer_name = request.class_name(),
      .subclass_id = gameinterop::WorldEntities::SubclassId(request.unit()),
      .team = request.team(),
      .position = Floats(request.position()),
      .facing = Floats(request.facing()),
      .velocity = {},
      .health = request.health(),
      .max_health = max_health,
      .lane = request.has_lane() ? std::optional(request.lane()) : std::nullopt,
  };
  auto entity = (*world)->Spawn(target);
  if (!entity) return std::unexpected(entity.error());
  npcs_.insert(*entity);
  spawned_ = true;
  NpcResponse response;
  response.set_npc(*entity);
  return response;
}

std::expected<NpcStateResponse, std::string> Game::ReadNpc(const NpcRequest& request) {
  auto world = World();
  if (!world) return std::unexpected(world.error());
  auto sample = (*world)->ReadNpc(request.npc());
  if (!sample) return std::unexpected(sample.error());
  NpcStateResponse response;
  if (!*sample) return response;
  const auto& state = (*sample)->state;
  auto* npc = response.mutable_state();
  SetVector(npc->mutable_position(), state.position);
  SetAngles(npc->mutable_facing(), state.facing);
  npc->set_health(state.health);
  npc->set_max_health(state.max_health);
  npc->set_team(state.team);
  return response;
}

std::expected<AliveResponse, std::string> Game::MoveNpc(const MoveNpcRequest& request) {
  auto world = World();
  if (!world) return std::unexpected(world.error());
  return Alive((*world)->Move(request.npc(), Floats(request.position()), Floats(request.facing()),
                              Floats(request.velocity())));
}

std::expected<AliveResponse, std::string> Game::SetNpcHealth(const SetNpcHealthRequest& request) {
  auto world = World();
  if (!world) return std::unexpected(world.error());
  const int32_t max_health = request.has_max_health() ? request.max_health() : request.health();
  return Alive((*world)->SetHealth(request.npc(), request.health(), max_health));
}

std::expected<AliveResponse, std::string> Game::RemoveNpc(const NpcRequest& request) {
  auto world = World();
  if (!world) return std::unexpected(world.error());
  npcs_.erase(request.npc());
  return Alive((*world)->RemoveNpc(request.npc()));
}

std::expected<PickupResponse, std::string> Game::CreatePickup(const CreatePickupRequest& request) {
  auto world = World();
  if (!world) return std::unexpected(world.error());
  const auto kind = request.kind() == PICKUP_KIND_URN
                        ? gameinterop::WorldEntities::Pickup::kUrn
                        : gameinterop::WorldEntities::Pickup::kMovementBuff;
  auto entity = (*world)->CreatePickup(kind, Floats(request.position()));
  if (!entity) return std::unexpected(entity.error());
  npcs_.insert(*entity);
  PickupResponse response;
  response.set_pickup(*entity);
  return response;
}

std::expected<AliveResponse, std::string> Game::PickupPresent(const PickupRequest& request) {
  auto world = World();
  if (!world) return std::unexpected(world.error());
  auto sample = (*world)->ReadPickup(request.pickup());
  if (!sample) return std::unexpected(sample.error());
  return Alive(sample->has_value());
}

std::expected<AliveResponse, std::string> Game::RemovePickup(const PickupRequest& request) {
  auto world = World();
  if (!world) return std::unexpected(world.error());
  npcs_.erase(request.pickup());
  return Alive((*world)->RemoveNpc(request.pickup()));
}

std::expected<CountResponse, std::string> Game::RemoveEntities(
    const RemoveEntitiesRequest& request) {
  auto world = World();
  if (!world) return std::unexpected(world.error());
  auto removed = (*world)->Remove(request.class_name());
  if (!removed) return std::unexpected(removed.error());
  CountResponse response;
  response.set_count(static_cast<int32_t>(*removed));
  return response;
}

std::expected<void, std::string> Game::ClearMap() {
  auto world = World();
  if (!world) return std::unexpected(world.error());
  auto damage = services_.Damage();
  if (!damage) return std::unexpected(damage.error());
  return (*world)->ClearAuthored(**damage);
}

std::expected<void, std::string> Game::Damage(const HitOptions& request) {
  auto damage = services_.Damage();
  if (!damage) return std::unexpected(damage.error());
  auto victim = Entity(request.victim());
  if (!victim) return std::unexpected(victim.error());
  // A hit nobody dealt is a hazard: it may kill and credits no one.
  if (request.attacker() == 0) return (*damage)->Hazard(*victim, request.amount());
  auto attacker = Entity(request.attacker());
  if (!attacker) return std::unexpected(attacker.error());
  auto inflictor = request.inflictor() == 0 ? attacker : Entity(request.inflictor());
  if (!inflictor) return std::unexpected(inflictor.error());
  const int32_t hit_group = request.has_hit_group() ? request.hit_group() : -1;
  void* ability = nullptr;
  if (request.has_ability()) {
    auto found = Entity(request.ability());
    if (!found) return std::unexpected(found.error());
    ability = *found;
  }
  return (*damage)->Hit(*victim, *inflictor, *attacker, ability, request.amount(), hit_group);
}

std::expected<void, std::string> Game::AdjustSouls(const AdjustSoulsRequest& request) {
  auto currency = services_.Currency();
  if (!currency) return std::unexpected(currency.error());
  if (auto sample = Live(request.player()); !sample) return std::unexpected(sample.error());
  auto adjusted =
      observer_.AdjustSouls(request.player(), request.delta(), request.silent(), *currency);
  if (!adjusted) return std::unexpected(adjusted.error());
  return {};
}

std::expected<void, std::string> Game::StartingSouls(const StartingSoulsRequest& request) {
  auto currency = services_.Currency();
  if (!currency) return std::unexpected(currency.error());
  if (auto sample = Live(request.player()); !sample) return std::unexpected(sample.error());
  auto prepared = observer_.PrepareStartingSouls(request.player(), request.souls(), *currency);
  if (!prepared) return std::unexpected(prepared.error());
  return {};
}

std::expected<void, std::string> Game::RestoreHero(const RestoreHeroRequest& request) {
  const auto& source = request.target();
  RestoreTarget target{
      .position = Floats(source.position()),
      .angles = Floats(source.facing()),
      .fresh = source.fresh(),
      .health = source.health(),
      .max_health = source.max_health(),
  };
  if (source.has_level()) target.level = source.level();
  if (!source.upgrade_bonuses().empty()) {
    if (source.upgrade_bonuses_size() != 3) {
      return std::unexpected("upgrade bonuses hold exactly three values");
    }
    target.upgrade_bonuses = {source.upgrade_bonuses(0), source.upgrade_bonuses(1),
                              source.upgrade_bonuses(2)};
  }
  for (const auto& ability : source.abilities()) {
    if (!ability.has_slot() || ability.slot() < 0 || ability.slot() > UINT16_MAX) {
      return std::unexpected("each ability needs its slot");
    }
    target.abilities.push_back({.subclass_id = ability.id(),
                                .slot = static_cast<uint16_t>(ability.slot()),
                                .upgrade_info = ability.state()});
  }
  if (source.replace_items()) {
    auto& items = target.items.emplace();
    for (const auto& item : source.items()) {
      gameinterop::ItemTarget out{.subclass_id = item.id(), .upgrade_info = item.state()};
      if (item.has_slot()) {
        if (item.slot() < 0 || item.slot() > UINT16_MAX) {
          return std::unexpected("an item slot is out of range");
        }
        out.slot = static_cast<uint16_t>(item.slot());
      }
      items.push_back(out);
    }
  }
  for (const auto& timer : source.timers()) {
    target.timers.push_back({.subclass_id = timer.id(),
                             .charges = timer.charges(),
                             .cooldown = {timer.cooldown_start(), timer.cooldown_end()},
                             .recharge = {timer.recharge_start(), timer.recharge_end()}});
  }

  auto sample = Live(request.player());
  if (!sample) return std::unexpected(sample.error());
  auto definitions = services_.Abilities();
  if (!definitions) return std::unexpected(definitions.error());
  auto server = services_.Server();
  if (!server) return std::unexpected(server.error());
  const auto* abilities = *definitions;
  const auto* engine = *server;
  HeroRestore::Engine access{
      .apply = [this](const HeroRestore::Sample& pawn, const RestoreTarget& target,
                      HeroRestore::Stage stage) { return ApplyRestore(pawn, target, stage); },
      .abilities = [this](int32_t slot) { return observer_.CurrentAbilitiesForSlot(slot); },
      .definition = [abilities](uint32_t id) { return abilities->Find(id); },
      .clock = [engine] { return engine->ReadClock(); },
      .set_timers =
          [this](int32_t slot, std::span<const HeroRestore::Ability> timers) {
            return observer_.ApplyAbilityTimers(slot, timers);
          },
      .ready_timers = [this](int32_t slot) { return observer_.FreshAbilityTargets(slot); },
  };
  auto restore = HeroRestore::Create(*sample, std::move(target), std::move(access));
  if (!restore) return std::unexpected(restore.error());
  restores_.insert_or_assign(request.player(), std::move(*restore));
  return {};
}

bool Game::ApplyRestore(const HeroRestore::Sample& sample, const RestoreTarget& target,
                        HeroRestore::Stage stage) {
  const auto log = [](std::string_view what, const std::string& error) {
    std::cerr << "[modlock] hero restore: " << what << ": " << error << "\n";
    return false;
  };
  const auto slot = sample.slot;
  void* pawn = observer_.PawnForSlot(slot);
  if (stage == HeroRestore::Stage::kBuild) {
    if (target.level && !gameinterop::RestorePawnLevel(pawn, *target.level)) return false;
    auto definitions = services_.Abilities();
    if (!definitions) return log("abilities", definitions.error());
    auto items = services_.Items();
    if (!items) return log("items", items.error());
    if (target.items) {
      if (auto done = observer_.ReconcileItems(slot, *target.items, **definitions, **items);
          !done) {
        return log("items", done.error());
      }
    }
    if (!target.abilities.empty()) {
      auto create = services_.CreateAbility();
      if (!create) return log("abilities", create.error());
      if (auto done = observer_.ReconcileAbilities(slot, target.abilities, **definitions, *create,
                                                   (*items)->set_bits);
          !done) {
        return log("abilities", done.error());
      }
    }

    // Item and ability calls can replace the pawn, so borrow it again.
    if (!observer_.CurrentAbilitiesForSlot(slot)) return false;
    pawn = observer_.PawnForSlot(slot);
    if (!target.fresh && target.upgrade_bonuses) {
      if (auto done = gameinterop::RestorePawnUpgradeBonuses(pawn, *target.upgrade_bonuses);
          !done) {
        return log("upgrade bonuses", done.error());
      }
    }
    return true;
  }

  if (stage == HeroRestore::Stage::kPawn) {
    if (target.fresh) {
      if (!sample.effective_max_health || *sample.effective_max_health <= 0) return false;
      auto server = services_.Server();
      if (!server) return log("stamina", server.error());
      auto clock = (*server)->ReadClock();
      if (!clock) return log("stamina", clock.error());
      if (auto done = observer_.RestorePracticeStamina(slot, clock->current_time); !done) {
        return log("stamina", done.error());
      }
      pawn = observer_.PawnForSlot(slot);
    }
    const auto health = target.fresh ? *sample.effective_max_health : target.health;
    const auto max_health = target.fresh ? sample.max_health : target.max_health;
    if (!gameinterop::RestorePawnHealth(pawn, health, max_health)) return false;
  }

  // A bot has no camera to turn; its hero faces the angles and its AI aims.
  auto teleport = services_.Teleport();
  if (!teleport) return log("pose", teleport.error());
  pawn = observer_.PawnForSlot(slot);
  if (pawn == nullptr) return false;
  if (sample.is_bot) {
    gameinterop::TeleportEntity(pawn, target.position, target.angles, {});
  } else {
    (*teleport)(nullptr, pawn, target.position.data(), target.angles.data());
  }

  // A teleport recomputes the hero's modifier states, dropping held ones.
  ApplyHolds();
  return true;
}

void Game::TickRestores() {
  if (npc_restore_) {
    if (auto result = npc_restore_->Tick()) {
      npcs_restored_ = *result ? std::string() : result->error();
      npc_restore_.reset();
    }
  }
  if (restores_.empty()) return;
  ++frames_;
  for (auto it = restores_.begin(); it != restores_.end();) {
    auto result = it->second.Tick(observer_.Observe(it->first), frames_);
    if (!result) {
      ++it;
      continue;
    }
    if (restored_.size() < kMaxChanges) {
      restored_.push_back({it->first, *result ? std::string() : result->error()});
    }
    it = restores_.erase(it);
  }
}

std::expected<void, std::string> Game::RestoreNpcs(const RestoreNpcsRequest& request) {
  std::vector<NpcRestore::Target> targets;
  for (const auto& npc : request.npcs()) {
    targets.push_back({
        .designer_name = npc.class_name(),
        .subclass_id = npc.id(),
        .team = npc.team(),
        .position = Floats(npc.position()),
        .facing = Floats(npc.facing()),
        .velocity = Floats(npc.velocity()),
        .health = npc.health(),
        .max_health = npc.max_health(),
        .lane = npc.has_lane() ? std::optional(npc.lane()) : std::nullopt,
    });
  }
  auto world = World();
  if (!world) return std::unexpected(world.error());
  auto* entities = *world;
  auto restore = NpcRestore::Create(
      std::move(targets), {
                              .restore =
                                  [entities](std::span<const NpcRestore::Target> targets) {
                                    return entities->Restore(targets);
                                  },
                              .finish = [entities] { return entities->FinishRestore(); },
                              .read = [entities] { return entities->Read(); },
                          });
  if (!restore) return std::unexpected(restore.error());
  npc_restore_ = std::move(*restore);
  return {};
}

std::expected<void, std::string> Game::Pause(const PauseRequest& request) {
  return SetPaused(request.paused());
}

std::expected<void, std::string> Game::SetPaused(bool paused) {
  auto pause = services_.Pause();
  if (!pause) return std::unexpected(pause.error());
  if (auto set = (*pause)->Set(paused); !set) return set;
  paused_ = paused;
  return {};
}

std::expected<MatchClockResponse, std::string> Game::MatchClock() {
  auto clock = services_.Clock();
  if (!clock) return std::unexpected(clock.error());
  auto server = services_.Server();
  if (!server) return std::unexpected(server.error());
  auto now = (*server)->ReadClock();
  if (!now) return std::unexpected(now.error());
  auto seconds = (*clock)->Read(now->current_time, now->interval);
  if (!seconds) return std::unexpected(seconds.error());
  MatchClockResponse response;
  response.set_seconds(*seconds);
  return response;
}

std::expected<void, std::string> Game::HoldMatchClock(const HoldMatchClockRequest& request) {
  if (!request.has_seconds()) {
    held_clock_.reset();
    return {};
  }
  if (auto clock = services_.Clock(); !clock) return std::unexpected(clock.error());
  held_clock_ = request.seconds();
  HoldClock();
  return {};
}

void Game::HoldClock() {
  if (!held_clock_) return;
  auto clock = services_.Clock();
  auto server = services_.Server();
  if (!clock || !server) return;
  auto now = (*server)->ReadClock();
  if (!now) return;

  // A hold the game refuses ends with a log line instead of every frame.
  if (auto held = (*clock)->Restore(*held_clock_, now->current_time, now->interval); !held) {
    std::cerr << "[modlock] match clock hold ended: " << held.error() << "\n";
    held_clock_.reset();
  }
}

std::expected<RiftResponse, std::string> Game::Rift() {
  auto rift = services_.Rift();
  if (!rift) return std::unexpected(rift.error());
  auto state = (*rift)->Read();
  if (!state) return std::unexpected(state.error());
  RiftResponse response;
  auto* out = response.mutable_state();
  out->set_scoring_team(state->scoring_team);
  out->set_scoring_time(state->scoring_time);
  out->set_cash_in_started(state->cash_in_started);
  out->set_give_up_time(state->give_up_time);
  out->set_next_spawn(state->next_spawn);
  out->set_spawn_window(state->spawn_window);
  return response;
}

std::expected<void, std::string> Game::StartRift(const StartRiftRequest& request) {
  auto rift = services_.Rift();
  if (!rift) return std::unexpected(rift.error());
  return (*rift)->StartAt(Floats(request.position()));
}

std::expected<void, std::string> Game::MoveEntity(const MoveEntityRequest& request) {
  auto entity = Entity(request.entity());
  if (!entity) return std::unexpected(entity.error());
  const auto position = Floats(request.position());
  const auto facing = Floats(request.facing());
  const auto velocity = Floats(request.velocity());
  gameinterop::TeleportEntity(*entity, position.data(),
                              request.has_facing() ? facing.data() : nullptr,
                              request.has_velocity() ? velocity.data() : nullptr);
  return {};
}

std::expected<void, std::string> Game::EmitSound(const EmitSoundRequest& request) {
  auto sound = services_.Sound();
  if (!sound) return std::unexpected(sound.error());
  auto entity = Entity(request.entity());
  if (!entity) return std::unexpected(entity.error());
  if (!(*sound)->Emit(*entity, request.sound().c_str())) {
    return std::unexpected("the game did not play " + request.sound());
  }
  return {};
}

std::expected<void, std::string> Game::Kill(const PlayerRequest& request) {
  auto damage = services_.Damage();
  if (!damage) return std::unexpected(damage.error());
  auto sample = Live(request.player());
  if (!sample) return std::unexpected(sample.error());
  if (sample->health <= 0) return {};
  return (*damage)->Kill(observer_.PawnForSlot(request.player()));
}

std::expected<void, std::string> Game::SetVelocity(const SetVelocityRequest& request) {
  if (auto sample = Live(request.player()); !sample) return std::unexpected(sample.error());
  gameinterop::SetEntityVelocity(observer_.PawnForSlot(request.player()),
                                 Floats(request.velocity()));
  return {};
}

std::expected<ButtonsResponse, std::string> Game::Buttons(const PlayerRequest& request) {
  auto pawn_field = services_.Field("CBasePlayerController", "m_hPawn");
  if (!pawn_field) return std::unexpected(pawn_field.error());
  auto hero_field = services_.Field("CCitadelPlayerController", "m_hHeroPawn");
  if (!hero_field) return std::unexpected(hero_field.error());
  auto movement = services_.Field("CBasePlayerPawn", "m_pMovementServices");
  if (!movement) return std::unexpected(movement.error());
  auto buttons = services_.Field("CPlayer_MovementServices", "m_nButtons");
  if (!buttons) return std::unexpected(buttons.error());
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) return std::unexpected(entities.error());

  // A player's controller is the entity at index slot + 1. A dead player's
  // input reaches the pawn that observes, so both pawns are read.
  const auto all = gameinterop::EntityInstances(*entities);
  const auto controller = std::ranges::find_if(all, [&](void* entity) {
    const auto handle = gameinterop::ReferenceHandleOf(entity);
    return handle && static_cast<int32_t>(*handle & 0x7fff) == request.player() + 1;
  });
  if (controller == all.end()) return std::unexpected("the player is not connected");
  ButtonsResponse response;
  for (const auto& field : {*pawn_field, *hero_field}) {
    uint32_t handle = 0;
    std::memcpy(&handle, static_cast<unsigned char*>(*controller) + field.offset, sizeof(handle));
    auto* pawn = static_cast<unsigned char*>(gameinterop::EntityInstance(*entities, handle));
    if (pawn == nullptr) continue;
    unsigned char* services = nullptr;
    std::memcpy(&services, pawn + movement->offset, sizeof(services));
    if (services == nullptr) continue;
    // InButtonState holds its vtable, then the held mask.
    uint64_t held = 0;
    std::memcpy(&held, services + buttons->offset + sizeof(void*), sizeof(held));
    response.set_buttons(response.buttons() | held);
  }
  return response;
}

std::expected<void, std::string> Game::WatchMovement(const WatchMovementRequest& request) {
  if (request.player() < 0 || request.player() >= kMaxPlayers) {
    return std::unexpected("no player has that slot");
  }
  if (request.watch()) {
    movers_.insert(request.player());
  } else {
    movers_.erase(request.player());
  }
  return {};
}

std::vector<MovementSample> Game::Movement(std::span<const MovementFact> facts) {
  std::vector<MovementSample> samples;
  for (const auto slot : movers_) {
    const auto sample = observer_.Observe(slot);
    if (!sample || sample->health <= 0 || !sample->movement) continue;
    const auto& movement = *sample->movement;
    if (!movement.abs_velocity || !movement.grounded_by_handle) continue;
    auto& out = samples.emplace_back();
    out.set_player(slot);
    out.set_pawn(sample->pawn_handle);
    SetVector(out.mutable_position(), {static_cast<float>(sample->x), static_cast<float>(sample->y),
                                       static_cast<float>(sample->z)});
    SetVector(out.mutable_velocity(), *movement.abs_velocity);
    out.set_grounded(*movement.grounded_by_handle);
    out.set_sliding(movement.sliding);
    out.set_mantling(movement.mantling);
    out.set_climbing(movement.climbing);
    out.set_dashing(movement.dashing);
    out.set_jump_ability(movement.jump_ability_handle.value_or(0));
    out.set_wall_jumps(movement.consecutive_wall_jumps.value_or(0));
    out.set_mantle_ability(movement.mantle_ability_handle.value_or(0));
    out.set_mantle_start(movement.mantle_start_time.value_or(0));
    if (movement.wall_contact_position) {
      SetVector(out.mutable_wall_contact(), *movement.wall_contact_position);
    }
    if (movement.current_wall_normal) {
      SetVector(out.mutable_wall_normal(), *movement.current_wall_normal);
    }
    if (movement.wall_jump_normal_used) {
      SetVector(out.mutable_wall_jump_normal(), *movement.wall_jump_normal_used);
    }
    out.set_wall_jump_facing(movement.wall_jump_facing.value_or(0));
    if (movement.last_time_on_zipline) out.set_zipline_time(*movement.last_time_on_zipline);
    for (const auto& fact : facts) {
      if (fact.pawn != sample->pawn_handle) continue;
      out.add_actions(fact.action);
      if (fact.action == MOVEMENT_ACTION_ABILITY_EXECUTED) out.add_casts(fact.ability);
    }
  }
  return samples;
}

std::expected<void, std::string> Game::WatchProjectiles(const ProjectileOptions& request) {
  projectiles_ = request;
  if (!projectiles_.names().empty()) services_.Projectiles();
  return {};
}

std::vector<Game::Hero> Game::Heroes() {
  std::vector<Hero> heroes;
  for (int32_t slot = 0; slot < kMaxPlayers; ++slot) {
    const auto sample = observer_.Observe(slot);
    if (!sample || sample->health <= 0) continue;
    Hero hero{.slot = slot,
              .handle = sample->pawn_handle,
              .pawn = observer_.PawnForSlot(slot),
              .feet = {float(sample->x), float(sample->y), float(sample->z)}};
    if (sample->movement) hero.velocity = sample->movement->abs_velocity;
    heroes.push_back(hero);
  }
  return heroes;
}

void Game::TrackProjectiles() {
  if (projectiles_.names().empty()) {
    projectiles_seen_.clear();
    return;
  }
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) return;
  std::set<uint32_t> seen;
  for (void* entity : gameinterop::EntityInstances(*entities)) {
    const auto name = DesignerName(entity);
    if (!Watches(projectiles_, name)) continue;
    const auto handle = gameinterop::ReferenceHandleOf(entity);
    if (!handle) continue;
    seen.insert(*handle);
    if (projectiles_seen_.contains(*handle) || launches_.size() >= kMaxChanges) continue;
    LaunchEvent launch;
    launch.set_entity(*handle);
    launch.set_name(std::string(name));
    const auto [owner, player] = Owner(entity);
    launch.set_owner(owner);
    launch.set_player(player);
    if (const auto origin = Origin(entity)) SetVector(launch.mutable_position(), *origin);
    launches_.push_back(std::move(launch));
  }
  projectiles_seen_ = std::move(seen);
}

std::optional<ImpactEvent> Game::Watching(void* entity, const gameinterop::TraceResult& contact) {
  const auto name = DesignerName(entity);
  if (!Watches(projectiles_, name)) return std::nullopt;
  ImpactEvent event;
  event.set_entity(gameinterop::ReferenceHandleOf(entity).value_or(0));
  event.set_name(std::string(name));
  const auto [owner, player] = Owner(entity);
  event.set_owner(owner);
  event.set_player(player);
  SetVector(event.mutable_start(), contact.start);
  SetVector(event.mutable_end(), contact.end);
  SetVector(event.mutable_position(), contact.position);
  event.set_hit(contact.entity_handle.value_or(0));
  return event;
}

std::pair<uint32_t, int32_t> Game::Owner(void* entity) {
  auto field = services_.Field("CBaseEntity", "m_hOwnerEntity");
  if (!field) return {0, -1};
  uint32_t owner = 0;
  std::memcpy(&owner, static_cast<unsigned char*>(entity) + field->offset, sizeof(owner));
  if (owner == UINT32_MAX) return {0, -1};
  for (int32_t slot = 0; slot < kMaxPlayers; ++slot) {
    const auto sample = observer_.Observe(slot);
    if (sample && sample->pawn_handle == owner) return {owner, slot};
  }
  return {owner, -1};
}

std::optional<std::array<float, 3>> Game::Origin(void* entity) {
  auto body = services_.Field("CBaseEntity", "m_CBodyComponent");
  auto node = services_.Field("CBodyComponent", "m_pSceneNode");
  auto origin = services_.Field("CGameSceneNode", "m_vecAbsOrigin");
  if (!body || !node || !origin) return std::nullopt;
  unsigned char* component = nullptr;
  std::memcpy(&component, static_cast<unsigned char*>(entity) + body->offset, sizeof(component));
  if (component == nullptr) return std::nullopt;
  unsigned char* scene = nullptr;
  std::memcpy(&scene, component + node->offset, sizeof(scene));
  if (scene == nullptr) return std::nullopt;
  std::array<float, 3> position{};
  std::memcpy(position.data(), scene + origin->offset, sizeof(position));
  return position;
}

std::expected<TraceResponse, std::string> Game::Trace(const TraceOptions& request) {
  auto trace = engine_->Trace();
  if (!trace) return std::unexpected(trace.error());
  if (!(*trace)->IsReady()) return std::unexpected("the world's collision is not ready");
  gameinterop::TraceOptions options;
  if (request.layers() != 0) options.interacts_with = request.layers();
  options.interacts_exclude = request.exclude();
  // The game's trace skips at most two entities.
  if (request.ignore_size() > static_cast<int>(options.ignored_entities.size())) {
    return std::unexpected("a trace ignores at most two entities");
  }
  std::ranges::copy(request.ignore(), options.ignored_entities.begin());
  auto result = (*trace)->Query(Floats(request.start()), Floats(request.end()),
                                gameinterop::TraceLine{}, options);
  if (!result) return std::unexpected(result.error());
  TraceResponse response;
  if (!result->DidHit()) return response;
  auto* hit = response.mutable_hit();
  SetVector(hit->mutable_position(), result->position);
  SetVector(hit->mutable_normal(), result->normal);
  hit->set_start_solid(result->start_in_solid);
  hit->set_entity(result->entity_handle.value_or(0));
  return response;
}

std::expected<ObjectResponse, std::string> Game::CreateParticle(const ParticleOptions& request) {
  auto effects = services_.Effects();
  if (!effects) return std::unexpected(effects.error());
  render::ParticleSettings settings{
      .resource = request.resource(),
      .origin = request.position(),
      .angles = Floats(request.facing()),
  };
  if (request.color() != 0) {
    // The particle takes its color as 0xAABBGGRR.
    settings.tint = render::ParticleTint{.rgba = std::byteswap(request.color()),
                                         .control_point = request.tint_point()};
  }
  if (request.has_point()) {
    settings.data = render::ParticleData{.control_point = request.point().index(),
                                         .value = Floats(request.point().position())};
  }
  if (request.parent() != 0) settings.parent = request.parent();
  auto particle = (*effects)->CreateParticle(settings);
  if (!particle) return std::unexpected(particle.error());
  return Keep(std::unique_ptr<render::WorldEffect>(std::move(*particle)));
}

std::expected<ObjectResponse, std::string> Game::CreateFog(const FogOptions& request) {
  // kDrawDistance reaches the far edges of the largest map.
  constexpr float kDrawDistance = 16384;
  auto effects = services_.Effects();
  if (!effects) return std::unexpected(effects.error());
  if (!fog_) {
    auto controller = (*effects)->CreateFogController(request.position(), kDrawDistance);
    if (!controller) return std::unexpected(controller.error());
    fog_ = std::move(*controller);
  }
  const uint32_t color = request.color();
  const std::array<uint8_t, 3> tint{static_cast<uint8_t>(color >> 24),
                                    static_cast<uint8_t>(color >> 16),
                                    static_cast<uint8_t>(color >> 8)};
  auto volume = (*effects)->CreateFogVolume(request.position(), Floats(request.facing()),
                                            Floats(request.mins()), Floats(request.maxs()),
                                            request.strength(), tint);
  if (!volume) return std::unexpected(volume.error());
  return Keep(std::move(*volume));
}

std::expected<void, std::string> Game::Heal(const HealRequest& request) {
  auto sample = Live(request.player());
  if (!sample) return std::unexpected(sample.error());
  if (sample->health <= 0) return std::unexpected("the player's hero is dead");
  const int32_t maximum = sample->effective_max_health.value_or(sample->max_health);
  const int32_t health = std::min(maximum, sample->health + request.amount());
  if (health <= sample->health) return {};
  if (!gameinterop::RestorePawnHealth(observer_.PawnForSlot(request.player()), health,
                                      sample->max_health)) {
    return std::unexpected("cannot heal the player's hero");
  }
  return {};
}

std::expected<void, std::string> Game::ScreenEffect(const ScreenEffectRequest& request) {
  auto messages = services_.Messages();
  if (!messages) return std::unexpected(messages.error());
  auto owner = ScreenOwner(request.player());
  if (!owner) return std::unexpected(owner.error());
  auto state = ScreenState(request.effect());
  if (!state) return std::unexpected(state.error());
  const auto& timing = request.timing();
  return (*messages)->ScreenEffect(request.player(), *owner, *state,
                                   {.delay = timing.delay(),
                                    .fade_in = timing.fade_in(),
                                    .hold = timing.hold(),
                                    .fade_out = timing.fade_out()});
}

std::expected<void, std::string> Game::ClearScreenEffect(const ClearScreenEffectRequest& request) {
  auto messages = services_.Messages();
  if (!messages) return std::unexpected(messages.error());
  auto owner = ScreenOwner(request.player());
  if (!owner) return std::unexpected(owner.error());
  auto state = ScreenState(request.effect());
  if (!state) return std::unexpected(state.error());
  return (*messages)->ClearScreenEffect(request.player(), *owner, *state);
}

std::expected<int32_t, std::string> Game::ScreenOwner(int32_t slot) {
  // The client drops an effect unless its owner is a live entity, so the
  // effect belongs to the player's hero.
  auto sample = Live(slot);
  if (!sample) return std::unexpected(sample.error());
  return static_cast<int32_t>(sample->pawn_handle & 0x7fff);
}

std::expected<void, std::string> Game::Sound(const SoundRequest& request) {
  auto sound = services_.Sound();
  if (!sound) return std::unexpected(sound.error());
  if (auto sample = Live(request.player()); !sample) return std::unexpected(sample.error());
  if (!(*sound)->Emit(observer_.PawnForSlot(request.player()), request.sound().c_str())) {
    return std::unexpected("the game did not play " + request.sound());
  }
  return {};
}

std::expected<gameinterop::WorldEntities*, std::string> Game::World() {
  if (!world_) {
    auto image = services_.Image();
    if (!image) return std::unexpected(image.error());
    auto schema = services_.Schema();
    if (!schema) return std::unexpected(schema.error());
    auto world = gameinterop::WorldEntities::Resolve(**image, *schema);
    if (!world) return std::unexpected(world.error());
    world_ = std::move(*world);
  }
  return &*world_;
}

std::expected<gameinterop::PawnObserver::Sample, std::string> Game::Live(int32_t slot) {
  auto sample = observer_.Observe(slot);
  if (!sample) return std::unexpected("the player has no live hero");
  return *sample;
}

std::expected<void*, std::string> Game::Entity(uint32_t handle) const {
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) return std::unexpected(entities.error());
  auto* entity = gameinterop::EntityInstance(*entities, handle);
  if (entity == nullptr) return std::unexpected("no live entity has that handle");
  return entity;
}

ObjectResponse Game::Keep(Object object) {
  const auto id = next_object_++;
  objects_.emplace(id, std::move(object));
  ObjectResponse response;
  response.set_object(id);
  return response;
}

}  // namespace modlock::wasm
