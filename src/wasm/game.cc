#include "wasm/game.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/console_variables.h"
#include "modlock/gameinterop/keyvalues.h"
#include "modlock/gameinterop/native_memory.h"
#include "modlock/gameinterop/player_selection.h"
#include "proto/modlock/hud.pb.h"

namespace modlock::wasm {
namespace {

// kMaxString bounds the bytes ReadField copies for a string field.
constexpr size_t kMaxString = 256;

// kMaxList bounds the elements of a list field the host reads or grows to, so
// a list the game has not initialized cannot size a huge copy.
constexpr int32_t kMaxList = 1 << 20;

// kMaxChanges bounds the button changes, and separately the launches, one
// mod has not yet received.
constexpr size_t kMaxChanges = 256;

// kDesignerNameOffset is CEntityIdentity's designer name pointer.
constexpr size_t kDesignerNameOffset = 0x20;

// IsRulesClass reports whether class_name is one of the classes that declare
// the game rules' fields.
bool IsRulesClass(const std::string& class_name) {
  return class_name == "CGameRules" || class_name == "CTeamplayRules" ||
         class_name == "CCitadelGameRules";
}

// ListHeader is the layout of a list field: a CUtlVector, which the game's
// networked lists share. The elements are an array of capacity entries.
struct ListHeader {
  int32_t size;
  int32_t padding;
  void* elements;
  int32_t capacity;
  int32_t grow;
};
static_assert(sizeof(ListHeader) == 24);

// ReadList copies the header of the list field at address.
std::expected<ListHeader, std::string> ReadList(const unsigned char* address) {
  ListHeader list;
  if (!gameinterop::ReadNative(address, &list, sizeof(list))) {
    return std::unexpected("cannot read the list");
  }
  if (list.size < 0 || list.size > list.capacity || list.capacity > kMaxList) {
    return std::unexpected("the list is not initialized");
  }
  return list;
}

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
  // Find the entity's identity, which holds its names.
  const auto* identity = static_cast<const unsigned char*>(gameinterop::IdentityOf(entity));
  if (identity == nullptr) {
    return {};
  }

  // Read the designer name pointer out of the identity.
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
  // Copy a scalar of type T into the field's bytes.
  std::array<unsigned char, 12> bytes{};
  auto put = [&bytes]<typename T>(T scalar) {
    std::memcpy(bytes.data(), &scalar, sizeof(scalar));
  };

  // Check that the value takes the form the field type reads as.
  auto expect = [&value](FieldValue::ValueCase form) -> std::expected<void, std::string> {
    if (value.value_case() != form) {
      return std::unexpected("the value does not fit the field type");
    }
    return {};
  };

  // Encode the value as the field type's bytes.
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
  if (!fits) {
    return std::unexpected(fits.error());
  }
  return bytes;
}

// FindHero resolves a request's hero by its identifier or its name.
template <typename Request>
std::expected<gameinterop::HeroDefinition, std::string> FindHero(
    const gameinterop::HeroDefinitions& heroes, const Request& request) {
  if (request.hero_case() == Request::kHeroId) {
    return heroes.Find(request.hero_id());
  }
  return heroes.Find(request.hero_name());
}

// Value converts an entity value to the world's form, or nothing when it has
// none. The result borrows value's text.
std::optional<gameinterop::EntityValue> Value(const EntityValue& value) {
  switch (value.value_case()) {
    case EntityValue::kBoolean:
      return value.boolean();
    case EntityValue::kInteger:
      return value.integer();
    case EntityValue::kNumber:
      return value.number();
    case EntityValue::kText:
      return std::string_view(value.text());
    case EntityValue::kVector: {
      const auto [x, y, z] = Floats(value.vector());
      return gameinterop::KeyValueVector{x, y, z};
    }
    case EntityValue::kColor: {
      const uint32_t color = value.color();
      return gameinterop::KeyValueColor{
          static_cast<uint8_t>(color >> 24), static_cast<uint8_t>(color >> 16),
          static_cast<uint8_t>(color >> 8), static_cast<uint8_t>(color)};
    }
    case EntityValue::VALUE_NOT_SET:
      break;
  }
  return std::nullopt;
}

// KeyValues converts spawn key values to the world's form. The result borrows
// the text of pairs.
std::expected<std::vector<gameinterop::EntityKeyValue>, std::string> KeyValues(
    const google::protobuf::RepeatedPtrField<KeyValue>& pairs) {
  std::vector<gameinterop::EntityKeyValue> result;
  result.reserve(pairs.size());
  for (const auto& pair : pairs) {
    auto value = Value(pair.value());
    if (!value) {
      return std::unexpected("key value " + pair.key() + " has no value");
    }
    result.push_back({.key = pair.key(), .value = *value});
  }
  return result;
}

// Alive answers whether the entity a call acted on is still there.
std::expected<AliveResponse, std::string> Alive(std::expected<bool, std::string> alive) {
  if (!alive) {
    return std::unexpected(alive.error());
  }
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
  if (remap.from() == 0) {
    return;
  }
  const bool held = input.buttons[0] & remap.from();
  for (auto& state : input.buttons) {
    const bool set = state & remap.from();
    state &= ~remap.from();
    if (set) {
      state |= remap.to();
    }
  }
  if (held && remap.repeat()) {
    input.buttons[1] |= remap.to();
  }
}

}  // namespace

std::expected<const gameinterop::EngineServer*, std::string> GameServices::Server() {
  if (!server_) {
    auto server = gameinterop::EngineServer::Resolve();
    if (!server) {
      return std::unexpected(server.error());
    }
    server_ = *server;
  }
  return &*server_;
}

std::expected<const gameinterop::NativeUserMessages*, std::string> GameServices::Messages() {
  if (!messages_) {
    auto messages = gameinterop::NativeUserMessages::TryCreate();
    if (!messages) {
      return std::unexpected(messages.error());
    }
    messages_ = *messages;
  }
  return &*messages_;
}

std::expected<void, std::string> GameServices::ServerCommand(std::string command) {
  // Hold the line until the world starts.
  command += '\n';
  if (!started_) {
    pending_commands_.push_back(std::move(command));
    return {};
  }

  // Resolve the engine server that runs console lines.
  auto server = Server();
  if (!server) {
    return std::unexpected(server.error());
  }

  // A line that sets a variable may name a development-only or cheat-protected
  // one, which the console otherwise refuses. Exposing the first word of any
  // other line finds no variable and changes nothing.
  if (auto variables = gameinterop::ConsoleVariables::Resolve()) {
    const auto name = command.substr(0, command.find_first_of(" \t\n"));
    static_cast<void>(variables->AllowServerChanges(name.c_str()));
  }

  // Run the line on the server console.
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
  if (!started_) {
    WorldStarting();
  }
  world_ready_ = true;
  if (input_wanted_) {
    Input();
  }
  if (impacts_wanted_) {
    Projectiles();
  }
}

std::expected<const gameinterop::ModuleImage*, std::string> GameServices::Image() {
  if (!image_) {
    auto image = gameinterop::MappedModuleImage::ForModule(L"server.dll");
    if (!image) {
      return std::unexpected(image.error());
    }
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
    if (!image) {
      return std::unexpected(image.error());
    }
    auto heroes = gameinterop::HeroDefinitions::Resolve(**image);
    if (!heroes) {
      return std::unexpected(heroes.error());
    }
    heroes_ = *heroes;
  }
  return &*heroes_;
}

std::expected<const gameinterop::PlayerSelectionCalls*, std::string> GameServices::Selection() {
  if (!selection_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto selection = gameinterop::PlayerSelectionCalls::Resolve(**image);
    if (!selection) {
      return std::unexpected(selection.error());
    }
    selection_ = *selection;
  }
  return &*selection_;
}

std::expected<const gameinterop::AbilityDefinitions*, std::string> GameServices::Abilities() {
  if (!abilities_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto abilities = gameinterop::AbilityDefinitions::Resolve(**image);
    if (!abilities) {
      return std::unexpected(abilities.error());
    }
    abilities_ = *abilities;
  }
  return &*abilities_;
}

std::expected<const gameinterop::ItemFunctions*, std::string> GameServices::Items() {
  if (!items_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto items = gameinterop::ItemFunctions::Resolve(**image);
    if (!items) {
      return std::unexpected(items.error());
    }
    items_ = *items;
  }
  return &*items_;
}

std::expected<const gameinterop::AbilitySlots*, std::string> GameServices::Slots() {
  if (!slots_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto schema = Schema();
    if (!schema) {
      return std::unexpected(schema.error());
    }
    auto slots = gameinterop::AbilitySlots::Resolve(**image, *schema);
    if (!slots) {
      return std::unexpected(slots.error());
    }
    slots_ = *slots;
  }
  return &*slots_;
}

std::expected<gameinterop::CreateAbility, std::string> GameServices::CreateAbility() {
  if (!create_ability_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto create = gameinterop::ResolveCreateAbility(**image);
    if (!create) {
      return std::unexpected(create.error());
    }
    create_ability_ = *create;
  }
  return create_ability_;
}

std::expected<gameinterop::RespawnPawn, std::string> GameServices::Respawn() {
  if (!respawn_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto respawn = gameinterop::ResolveRespawnPawn(**image);
    if (!respawn) {
      return std::unexpected(respawn.error());
    }
    respawn_ = *respawn;
  }
  return respawn_;
}

std::expected<gameinterop::ResetHeroPawn, std::string> GameServices::ResetHero() {
  if (!reset_hero_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto reset = gameinterop::ResolveResetHeroPawn(**image);
    if (!reset) {
      return std::unexpected(reset.error());
    }
    reset_hero_ = *reset;
  }
  return reset_hero_;
}

std::expected<gameinterop::TeleportClientCamera, std::string> GameServices::Teleport() {
  if (!teleport_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto teleport = gameinterop::ResolveTeleportClientCamera(**image);
    if (!teleport) {
      return std::unexpected(teleport.error());
    }
    teleport_ = *teleport;
  }
  return teleport_;
}

std::expected<const gameinterop::PawnMotion*, std::string> GameServices::Motion() {
  if (!motion_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto motion = gameinterop::ResolvePawnMotion(**image);
    if (!motion) {
      return std::unexpected(motion.error());
    }
    motion_ = *motion;
  }
  return &*motion_;
}

std::expected<const gameinterop::BotCreation*, std::string> GameServices::Bots() {
  if (!bots_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto bots = gameinterop::BotCreation::Resolve(**image);
    if (!bots) {
      return std::unexpected(bots.error());
    }
    bots_ = *bots;
    // Without the guard a solo bot match clears a bot's hero that a human
    // plays; the bots still join, so a failure costs only mirrored heroes.
    if (auto guard = gameinterop::BotHeroGuard::Install(**image)) {
      bot_heroes_ = std::move(*guard);
    } else {
      std::cerr << "mod bots: " << guard.error() << '\n';
    }
  }
  return &*bots_;
}

std::expected<render::WorldTextGameFactory*, std::string> GameServices::Text() {
  // Build the text factory once from the server module.
  if (!text_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto text = render::WorldTextGameFactory::TryCreate(**image);
    if (!text) {
      return std::unexpected(text.error());
    }
    text_ = std::move(*text);
  }

  // Each world has its own entity system.
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) {
    return std::unexpected(entities.error());
  }
  text_->SetEntitySystem(*entities);
  return text_.get();
}

std::expected<render::WorldEffectGameFactory*, std::string> GameServices::Effects() {
  // Build the effect factory once from the server module.
  if (!effects_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto effects = render::WorldEffectGameFactory::TryCreate(**image);
    if (!effects) {
      return std::unexpected(effects.error());
    }
    effects_ = std::move(*effects);
  }

  // Each world has its own entity system.
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) {
    return std::unexpected(entities.error());
  }
  effects_->SetEntitySystem(*entities);
  return effects_.get();
}

std::expected<const gameinterop::NativeDamage*, std::string> GameServices::Damage() {
  if (!damage_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto schema = Schema();
    if (!schema) {
      return std::unexpected(schema.error());
    }
    auto damage = gameinterop::NativeDamage::Resolve(**image, *schema);
    if (!damage) {
      return std::unexpected(damage.error());
    }
    damage_ = std::move(*damage);
  }
  return &*damage_;
}

std::expected<gameinterop::ModifyCurrency, std::string> GameServices::Currency() {
  if (!currency_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto currency = gameinterop::ResolveModifyCurrency(**image);
    if (!currency) {
      return std::unexpected(currency.error());
    }
    currency_ = *currency;
  }
  return currency_;
}

std::expected<const gameinterop::NativeSound*, std::string> GameServices::Sound() {
  if (!sound_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto sound = gameinterop::NativeSound::TryCreate(**image);
    if (!sound) {
      return std::unexpected(sound.error());
    }
    sound_ = std::move(*sound);
  }
  return &*sound_;
}

std::expected<const gameinterop::GamePause*, std::string> GameServices::Pause() {
  if (!pause_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto schema = Schema();
    if (!schema) {
      return std::unexpected(schema.error());
    }
    auto pause = gameinterop::GamePause::Resolve(**image, *schema);
    if (!pause) {
      return std::unexpected(pause.error());
    }
    pause_ = *pause;
  }
  return &*pause_;
}

std::expected<gameinterop::MatchClock*, std::string> GameServices::Clock() {
  if (!clock_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto schema = Schema();
    if (!schema) {
      return std::unexpected(schema.error());
    }
    auto clock = gameinterop::MatchClock::Resolve(**image, *schema);
    if (!clock) {
      return std::unexpected(clock.error());
    }
    clock_ = *clock;
  }
  return &*clock_;
}

std::expected<void*, std::string> GameServices::Rules() {
  // Resolve the server module the rules must live in.
  auto image = Image();
  if (!image) {
    return std::unexpected(image.error());
  }

  // Find the game's pointer to its current rules once.
  if (!rules_) {
    auto current = gameinterop::ResolveCurrentGameRules(**image);
    if (!current) {
      return std::unexpected(current.error());
    }
    rules_ = *current;
  }

  // Read the rules through that pointer.
  void* rules = nullptr;
  std::memcpy(&rules, rules_, sizeof(rules));
  if (!rules) {
    return std::unexpected("the game rules are not active");
  }

  // A rules object whose table leaves the server module is stale.
  uintptr_t table = 0;
  std::memcpy(&table, rules, sizeof(table));
  if (table < (*image)->base() || table - (*image)->base() >= (*image)->image_bytes().size()) {
    return std::unexpected("the game rules leave the server module");
  }
  return rules;
}

std::expected<gameinterop::KothRules*, std::string> GameServices::Rift() {
  if (!rift_) {
    auto image = Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto schema = Schema();
    if (!schema) {
      return std::unexpected(schema.error());
    }
    auto rift = gameinterop::KothRules::Resolve(**image, *schema);
    if (!rift) {
      return std::unexpected(rift.error());
    }
    rift_ = std::move(*rift);
  }
  return &*rift_;
}

std::expected<gameinterop::SchemaField, std::string> GameServices::Field(
    const std::string& class_name, const std::string& field) {
  // Answer a field resolved before from the cache.
  FieldKey key{class_name, field};
  if (auto found = fields_.find(key); found != fields_.end()) {
    return found->second;
  }

  // Resolve the field in the server's schema and cache it.
  auto schema = Schema();
  if (!schema) {
    return std::unexpected(schema.error());
  }
  auto resolved =
      gameinterop::SchemaFieldOf(*schema, "server.dll", class_name.c_str(), field.c_str());
  if (!resolved) {
    return std::unexpected(class_name + "." + field + ": " + resolved.error());
  }
  fields_.emplace(std::move(key), *resolved);
  return *resolved;
}

bool GameServices::Derives(const void* class_info, const std::string& class_name) {
  // Answer a class resolved before from the cache.
  auto key = std::pair{class_info, class_name};
  if (auto found = derives_.find(key); found != derives_.end()) {
    return found->second;
  }

  // Walk the class's bases and cache the answer.
  const bool derives = gameinterop::SchemaClassDerivesFrom(class_info, class_name);
  derives_.emplace(std::move(key), derives);
  return derives;
}

std::expected<GameServices::ModifierLayout, std::string> GameServices::Modifiers() {
  // Answer the layout resolved before.
  if (modifiers_) {
    return *modifiers_;
  }

  // Resolve the modifier property and its two state masks.
  auto property = Field("CBaseEntity", "m_pModifierProp");
  if (!property) {
    return std::unexpected(property.error());
  }
  auto mask = Field("CModifierProperty", "m_bvEnabledStateMask");
  if (!mask) {
    return std::unexpected(mask.error());
  }
  auto predicted = Field("CModifierProperty", "m_bvEnabledPredictedStateMask");
  if (!predicted) {
    return std::unexpected(predicted.error());
  }

  // Cache the offsets every modifier state read uses.
  modifiers_ = ModifierLayout{
      .property = property->offset,
      .mask = mask->offset,
      .predicted_mask = predicted->offset,
      .mask_size = std::min(mask->size, predicted->size),
  };
  return *modifiers_;
}

std::expected<int64_t, std::string> GameServices::ModifierState(const std::string& name) {
  // Answer a state resolved before from the cache.
  if (auto found = modifier_states_.find(name); found != modifier_states_.end()) {
    return found->second;
  }

  // Resolve the state in the server's enum and cache it.
  auto schema = Schema();
  if (!schema) {
    return std::unexpected(schema.error());
  }
  auto value =
      gameinterop::SchemaEnumValueOf(*schema, "server.dll", "EModifierState", name.c_str());
  if (!value) {
    return std::unexpected(name + ": " + value.error());
  }
  modifier_states_.emplace(name, *value);
  return *value;
}

void GameServices::Input() {
  // Install the hook once the world is ready to take input.
  input_wanted_ = true;
  if (input_ || !world_ready_) {
    return;
  }

  // Resolve the server module and schema the hook needs.
  auto image = Image();
  auto schema = Schema();
  if (!image || !schema) {
    return;
  }

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
    if (press == game->presses_.end()) {
      continue;
    }

    // A press is down and newly changed for this one input.
    input.buttons[0] |= press->second;
    input.buttons[1] |= press->second;
    input.buttons[2] &= ~press->second;
    game->presses_.erase(press);
  }
  for (auto* game : games_) {
    Remap(game->remap_, input);
  }
  return blocked;
}

void GameServices::Projectiles() {
  // Install the hook once the world is ready for impacts.
  impacts_wanted_ = true;
  if (impacts_ || !world_ready_) {
    return;
  }

  // Resolve the server module the hook patches.
  auto image = Image();
  if (!image) {
    return;
  }

  // Hook impacts, keeping the hook only when it installs.
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

std::expected<void, std::string> GameServices::Transmit() {
  // Install the transmit hook once.
  if (transmit_) {
    return {};
  }
  auto hook =
      gameinterop::TransmitHook::Install([this](gameinterop::TransmitSet& set) { Withhold(set); });
  if (!hook) {
    return std::unexpected(hook.error());
  }
  transmit_ = std::move(*hook);
  return {};
}

void GameServices::Withhold(gameinterop::TransmitSet& set) {
  // A text object's entity changes with its words, so each update reads the
  // current handles.
  for (auto* game : games_) {
    auto hidden = game->hidden_.find(set.Slot());
    if (hidden == game->hidden_.end()) {
      continue;
    }
    for (const auto id : hidden->second) {
      auto found = game->objects_.find(id);
      if (found == game->objects_.end()) {
        continue;
      }
      auto handle = std::visit([](const auto& object) { return object->Handle(); }, found->second);
      if (handle) {
        set.Withhold(*handle);
      }
    }
  }

  // The engine's pass already set the bits of entities sent to every client,
  // TransmitAlways ones included, so clearing them here still withholds them.
  for (auto* game : games_) {
    for (const auto& [entity, slots] : game->audiences_) {
      set.WithholdUnlessIn(entity, slots);
    }
  }
}

void GameServices::Impact(void* entity, const gameinterop::TraceResult& contact,
                          const gameinterop::ProjectileImpactHook::NativeImpact& native) {
  // Collect the games that watch this projectile.
  std::vector<std::pair<Game*, ImpactEvent>> watching;
  bool keep = false;
  for (auto* game : games_) {
    auto event = game->Watching(entity, contact);
    if (!event) {
      continue;
    }
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
  if (keep) {
    heroes = watching.front().first->Heroes();
  }
  native();
  for (const auto& hero : heroes) {
    if (hero.velocity) {
      gameinterop::SetEntityVelocity(hero.pawn, *hero.velocity);
    }
  }

  // Tell each watching game, if it is still loaded.
  for (auto& [game, event] : watching) {
    if (games_.contains(game)) {
      game->Impacted(event);
    }
  }
}

std::expected<void, std::string> GameServices::StepWith(gameinterop::MovementHook::Handler step) {
  // Allow one mod's movement handler at a time.
  if (step && step_) {
    return std::unexpected("another mod's movement runs");
  }
  step_ = std::move(step);

  // Install or remove the movement hook to match.
  auto steps = Steps();
  if (!steps) {
    step_ = nullptr;
  }
  return steps;
}

std::expected<void, std::string> GameServices::Steps() {
  // Keep the hook only while a mod or a steered bot moves heroes.
  const bool needed = step_ || std::ranges::any_of(games_, [](const Game* game) {
                        return game->Moving() || !game->steers_.empty();
                      });
  if (!needed) {
    steps_.reset();
    commands_.clear();
    return {};
  }

  // Install the hook the first time a mover needs it.
  if (steps_) {
    return {};
  }
  auto hook = gameinterop::MovementHook::Install(
      gameinterop::MovementHook::Module::kServer,
      [this](gameinterop::MovementCall& call) { return Step(call); });
  if (!hook) {
    return std::unexpected(hook.error());
  }
  steps_ = std::move(*hook);
  return {};
}

bool GameServices::Step(gameinterop::MovementCall& call) {
  // Note the ground the last step stood on.
  using gameinterop::MovementHook;
  if (call.ground_entity != MovementHook::kNoGround) {
    ground_ = call.ground_entity;
  }

  // Record the pawn's command for the mod to read.
  if (call.pawn) {
    // The tick's first command fixes its starting ground; later commands add
    // their presses and leave the last buttons and axes.
    auto& [command, stepped] = commands_[call.pawn];
    if (!std::exchange(stepped, true)) {
      command.set_grounded(call.ground_entity != MovementHook::kNoGround);
    }
    command.set_held(call.buttons[0]);
    command.set_changed(command.changed() | call.buttons[1]);
    command.set_scroll(command.scroll() | call.buttons[2]);
    command.set_forward(call.forward);
    command.set_left(call.left);
    command.set_up(call.up);
  }

  // Steer a bot that replays a recorded command.
  for (auto* game : games_) {
    for (auto& [slot, steered] : game->steers_) {
      if (steered.pawn != call.pawn) {
        continue;
      }

      // The recorded command drives the game's own movement, abilities and
      // animation for this step.
      auto& steering = steered.steering;
      auto& command = *steering.mutable_command();
      call.buttons = {command.held(), command.changed(), command.scroll()};
      call.forward = command.forward();
      call.left = command.left();
      call.up = command.up();
      // The recorded view steers the step and becomes the hero's aim; its yaw
      // turns the body, as Game::MovePlayer does. The bot's own command set
      // both before this step, and the eye angles alone leave the body at the
      // bot's yaw, so the hero would run sideways.
      call.angles = Floats(steering.facing());
      static_cast<void>(game->observer_.SetEyeAngles(call.pawn, call.angles));
      if (auto motion = Motion()) {
        const std::array<float, 3> body{0, call.angles[1], 0};
        (*motion)->set_angles(call.pawn, body.data());
      }
      // A press counts once, though the command stays for the ticks after.
      command.set_changed(0);
      command.set_scroll(0);

      // The step then ends exactly on the recorded pose, on the ground the
      // heroes stand on when its own step left it in the air.
      call.origin = Floats(steering.position());
      call.velocity = Floats(steering.velocity());
      const auto ground = call.ground_entity;
      if (!steering.grounded()) {
        call.ground_entity = MovementHook::kNoGround;
      } else if (ground == MovementHook::kNoGround) {
        call.ground_entity = ground_;
      }
      if (call.ground_entity != ground) {
        static_cast<void>(gameinterop::NotifyEntityStateChanged(call.pawn));
      }
      return true;
    }
  }
  return step_ && step_(call);
}

std::optional<MovementCommand> GameServices::Command(const void* pawn) {
  // Find the pawn's command for this tick.
  const auto found = commands_.find(pawn);
  if (found == commands_.end()) {
    return std::nullopt;
  }

  // Hand it over, keeping the held buttons and clearing the presses.
  auto& [command, stepped] = found->second;
  auto taken = command;
  command.set_changed(0);
  command.set_scroll(0);
  stepped = false;
  return taken;
}

Game::Game(GameServices& services, EngineHost* engine)
    : services_(services),
      engine_(engine),
      observer_(gameinterop::PawnObserver::LiveSeams(true), true) {
  services_.games_.insert(this);
}

Game::~Game() {
  services_.games_.erase(this);
  if (auto steps = services_.Steps(); !steps) {
    std::cerr << "mod movement: " << steps.error() << '\n';
  }
}

void Game::Frame() {
  // Apply the per-frame state each feature keeps.
  ApplyFreeForAll();
  ApplyHolds();
  HoldClock();
  TickRestores();
  TrackProjectiles();
  PruneAudiences();

  // Finish the entities spawned this frame together.
  if (!spawned_ || !world_) {
    return;
  }
  world_->FinishSpawns();
  spawned_ = false;
}

void Game::ApplyHolds() {
  std::erase_if(
      held_, [this](const auto& hold) { return !SetModifierState(hold.first, hold.second, true); });
  for (const auto& [slot, key] : held_modifiers_) {
    static_cast<void>(RenewModifier(slot, key));
  }
}

std::expected<void, std::string> Game::RenewModifier(int32_t slot, const std::string& key) {
  // Find the modifier the slot holds.
  const auto modifier = modifiers_.find(key);
  if (modifier == modifiers_.end()) {
    return std::unexpected("the modifier is not held");
  }

  // A dead hero has no modifiers to renew.
  const auto sample = observer_.Observe(slot);
  if (!sample || sample->health <= 0) {
    return {};
  }

  // Attach the modifier again if the game removed it.
  auto attached = modifier->second.Attached(observer_, slot);
  if (!attached) {
    return std::unexpected(attached.error());
  }
  if (*attached) {
    return {};
  }
  const auto ability = std::string_view(key).substr(0, key.find('/'));
  return modifier->second.Attach(observer_, slot, gameinterop::MakeMemberName(ability).hash);
}

void Game::ReleaseModifier(int32_t slot, const std::string& key) {
  // Drop the slot's hold.
  if (held_modifiers_.erase({slot, key}) == 0) {
    return;
  }

  // Restore the modifier once no slot holds it.
  if (std::ranges::any_of(held_modifiers_, [&](const auto& hold) { return hold.second == key; })) {
    return;
  }
  if (auto modifier = modifiers_.find(key); modifier != modifiers_.end()) {
    modifier->second.Restore();
    modifiers_.erase(modifier);
  }
}

void Game::ReleaseModifiers() {
  for (const auto& [key, modifier] : modifiers_) {
    modifier.Restore();
  }
  modifiers_.clear();
  held_modifiers_.clear();
}

std::vector<Game::ButtonChange> Game::TakeButtonChanges() {
  return std::exchange(changes_, {});
}

std::vector<Game::Restored> Game::TakeRestored() {
  return std::exchange(restored_, {});
}

std::optional<std::string> Game::TakeNpcsRestored() {
  return std::exchange(npcs_restored_, {});
}

void Game::WorldReady() {
  // Each lookup is kept once found; one that fails here is reported by the
  // call that needs it.
  static_cast<void>(services_.Heroes());
  static_cast<void>(services_.Selection());
  static_cast<void>(services_.Modifiers());
  static_cast<void>(services_.ModifierState("MODIFIER_STATE_HOLDING_IDOL"));
  observer_.Prepare();
}

void Game::WorldEnding() {
  // Remove the world's objects and forget its entities.
  RemoveWorld();
  world_.reset();
  spawned_ = false;

  // Drop the per-world player state.
  frozen_.clear();
  steers_.clear();
  held_.clear();
  ffa_held_.clear();

  // Release what the mod held in the world.
  ReleaseModifiers();
  presses_.clear();
  restores_.clear();
  npc_restore_.reset();
  held_clock_.reset();

  // Forget the paused clock and the observer's cached pawns.
  paused_ = false;
  observer_.Invalidate();
}

void Game::Leave(int32_t slot) {
  // Forget the slot's movement, input and visibility.
  ffa_failed_.erase(slot);
  frozen_.erase(slot);
  movers_.erase(slot);
  steers_.erase(slot);
  restores_.erase(slot);
  slot_blocked_.erase(slot);
  presses_.erase(slot);
  hidden_.erase(slot);

  // Whoever takes the slot next is a different player, who was never listed.
  if (slot >= 0 && slot < 64) {
    for (auto& [entity, slots] : audiences_) {
      slots &= ~(uint64_t{1} << slot);
    }
  }

  // Release the modifiers the slot held.
  std::vector<std::string> keys;
  for (const auto& [held, key] : held_modifiers_) {
    if (held == slot) {
      keys.push_back(key);
    }
  }
  for (const auto& key : keys) {
    ReleaseModifier(slot, key);
  }
}

void Game::Clear() {
  // Stop watching every player.
  for (const auto slot : observer_.WatchingSlots()) {
    PlayerRequest request;
    request.set_player(slot);
    static_cast<void>(Unwatch(request));
  }

  // Remove the world's objects and unfreeze the frozen heroes.
  RemoveWorld();
  for (const auto slot : frozen_) {
    if (observer_.Observe(slot)) {
      static_cast<void>(observer_.SetPreparationFrozen(slot, false));
    }
  }

  // Stop moving and steering heroes.
  frozen_.clear();
  movers_.clear();
  steers_.clear();

  // Releasing the movement hook cannot fail.
  static_cast<void>(services_.Steps());

  // Release held modifier states and the free-for-all.
  for (const auto& [entity, state] : held_) {
    static_cast<void>(SetModifierState(entity, state, false));
  }
  held_.clear();
  free_for_all_ = false;
  ffa_held_.clear();
  ffa_failed_.clear();
  ReleaseModifiers();

  // Release the input the mod blocked, remapped or watched.
  blocked_ = 0;
  slot_blocked_.clear();
  remap_.Clear();
  presses_.clear();
  watched_ = 0;
  changes_.clear();

  // Drop pending restores and their reports.
  restores_.clear();
  restored_.clear();
  npc_restore_.reset();
  npcs_restored_.reset();

  // Resume the clock and stop watching projectiles.
  held_clock_.reset();
  if (paused_) {
    static_cast<void>(SetPaused(false));
  }
  projectiles_.Clear();
}

void Game::RemoveWorld() {
  // Remove the text, effect and fog objects.
  for (auto& [id, object] : objects_) {
    std::visit([](auto& handle) { handle->Remove(); }, object);
  }
  objects_.clear();
  hidden_.clear();
  if (fog_) {
    fog_->Remove();
    fog_.reset();
  }

  // Remove the entities the mod created.
  if (world_) {
    for (const auto entity : created_) {
      static_cast<void>(world_->RemoveEntity(entity));
    }
  }
  created_.clear();
  audiences_.clear();

  // Only the engine's kick removes a bot's connection.
  if (auto server = services_.Server()) {
    for (const auto& [slot, name] : bots_) {
      if ((*server)->ServerCommand(("kick \"" + name + "\"\n").c_str())) {
        Depart(slot);
      }
    }
  }
  bots_.clear();
}

void Game::Depart(int32_t slot) {
  departing_[slot] = gameinterop::ConnectionTracker::StateForSlot(slot).generation;
}

bool Game::Departing() {
  std::erase_if(departing_, [](const auto& entry) {
    const auto state = gameinterop::ConnectionTracker::StateForSlot(entry.first);
    return !state.occupied || state.generation != entry.second;
  });
  return !departing_.empty();
}

std::expected<void, std::string> Game::ServerCommand(const ServerCommandRequest& request) {
  return services_.ServerCommand(request.command());
}

std::expected<void, std::string> Game::Chat(const ChatRequest& request) {
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  return (*messages)->Chat(request.player(), request.text());
}

std::expected<void, std::string> Game::CenterText(const CenterTextRequest& request) {
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  return (*messages)->CenterText(request.player(), request.text());
}

std::expected<void, std::string> Game::Announce(const AnnounceRequest& request) {
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  return (*messages)->Announce(request.player(), request.title(), request.text());
}

std::expected<PlayersResponse, std::string> Game::Players() {
  PlayersResponse players;
  for (int32_t slot = 0; slot < kMaxPlayers; ++slot) {
    const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
    if (!state.occupied) {
      continue;
    }
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
  // Sample the player's pawn; a slot without one answers no pawn.
  PawnResponse response;
  const auto sample = observer_.Observe(request.player());
  if (!sample) {
    return response;
  }

  // Copy the pawn's identity and health.
  auto* pawn = response.mutable_pawn();
  pawn->set_entity(sample->pawn_handle);
  pawn->set_hero(sample->hero_id);
  pawn->set_team(sample->team);
  pawn->set_health(sample->health);
  pawn->set_max_health(sample->max_health);

  // Copy the pawn's position and view.
  pawn->mutable_position()->set_x(sample->x);
  pawn->mutable_position()->set_y(sample->y);
  pawn->mutable_position()->set_z(sample->z);
  SetAngles(pawn->mutable_eye_angles(), sample->eye_angles);
  SetAngles(pawn->mutable_camera_angles(), sample->camera_angles);

  // Copy the optional readings the sample carries.
  if (sample->stamina) {
    pawn->set_stamina(sample->stamina->current);
    pawn->set_max_stamina(sample->stamina->max);
  }
  pawn->set_generation(sample->session_generation);
  if (sample->currencies) {
    pawn->set_souls((*sample->currencies)[0]);
  }

  // Copy the pawn's eye and velocity when the sample has them.
  if (sample->eye_position) {
    SetVector(pawn->mutable_eye_position(), *sample->eye_position);
  }
  if (sample->movement && sample->movement->abs_velocity) {
    SetVector(pawn->mutable_velocity(), *sample->movement->abs_velocity);
  }

  // Copy the hero's scoreboard line.
  if (sample->score) {
    auto* score = pawn->mutable_score();
    score->set_kills(sample->score->kills);
    score->set_deaths(sample->score->deaths);
    score->set_assists(sample->score->assists);
    score->set_last_hits(sample->score->last_hits);
    score->set_denies(sample->score->denies);
    score->set_hero_damage(sample->score->hero_damage);
    score->set_hero_healing(sample->score->hero_healing);
    score->set_net_worth(sample->score->net_worth);
  }

  // The game marks the hero carrying the urn with its holding-idol state.
  ModifierStateRequest holding;
  holding.set_entity(sample->pawn_handle);
  holding.set_state("MODIFIER_STATE_HOLDING_IDOL");
  if (const auto carrying = ModifierState(holding)) {
    pawn->set_carrying_urn(carrying->active());
  }
  return response;
}

std::expected<unsigned char*, std::string> Game::FieldAddress(void* entity,
                                                              const std::string& class_name,
                                                              const std::string& field,
                                                              size_t size) {
  // Resolve the field's offset and check it holds the requested type.
  auto found = services_.Field(class_name, field);
  if (!found) {
    return std::unexpected(found.error());
  }
  if (found->size < size) {
    return std::unexpected(class_name + "." + field + " is smaller than its requested type");
  }

  // The offset belongs to class_name, so the entity must be one.
  auto actual = gameinterop::SchemaClassOfEntity(entity, gameinterop::ReadNative);
  if (!actual) {
    return std::unexpected(actual.error());
  }

  // The game rules are not an entity: their proxy entity points to them and
  // sends their fields, so a rules field is read and written through it.
  if (IsRulesClass(class_name) && services_.Derives(*actual, "CGameRulesProxy")) {
    auto rules = services_.Rules();
    if (!rules) {
      return std::unexpected(rules.error());
    }
    return static_cast<unsigned char*>(*rules) + found->offset;
  }
  if (!services_.Derives(*actual, class_name)) {
    return std::unexpected("the entity is a " +
                           std::string(gameinterop::SchemaClassNameOf(*actual)) + ", not a " +
                           class_name);
  }
  return static_cast<unsigned char*>(entity) + found->offset;
}

std::expected<void, std::string> Game::StoreField(void* entity, const std::string& class_name,
                                                  const std::string& field, FieldType type,
                                                  const FieldValue& value) {
  // Encode the value and find the field it goes to.
  auto bytes = Encode(type, value);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  auto address = FieldAddress(entity, class_name, field, Size(type));
  if (!address) {
    return std::unexpected(address.error());
  }

  // Write the bytes into the entity.
  if (!gameinterop::WriteNative(*address, bytes->data(), Size(type))) {
    return std::unexpected("cannot write " + class_name + "." + field);
  }
  return {};
}

std::expected<FieldResponse, std::string> Game::ReadField(const ReadFieldRequest& request) {
  // Find the entity and the field's address.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  const auto size = Size(request.type());
  if (size == 0) {
    return std::unexpected("the field type is unknown");
  }
  auto address = FieldAddress(*entity, request.class_name(), request.field(), size);
  if (!address) {
    return std::unexpected(address.error());
  }

  // Copy through the operating system, so a field the entity lacks fails
  // instead of faulting the server.
  std::array<unsigned char, 12> bytes{};
  if (!gameinterop::ReadNative(*address, bytes.data(), Size(request.type()))) {
    return std::unexpected("cannot read " + request.class_name() + "." + request.field());
  }
  auto value = Decode(request.type(), bytes.data());
  if (!value) {
    return std::unexpected(value.error());
  }
  FieldResponse response;
  *response.mutable_value() = std::move(*value);
  return response;
}

std::expected<void, std::string> Game::WriteField(const WriteFieldRequest& request) {
  // Find the entity and store the value in its field.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto stored =
      StoreField(*entity, request.class_name(), request.field(), request.type(), request.value());
  if (!stored) {
    return stored;
  }

  // Tell the engine to send the changed field.
  if (!gameinterop::NotifyEntityStateChanged(*entity)) {
    return std::unexpected("cannot replicate " + request.class_name() + "." + request.field());
  }
  return {};
}

std::expected<FieldListResponse, std::string> Game::ReadFieldList(const ReadFieldRequest& request) {
  // Find the entity, the list field's address and its header.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  const auto stride = Size(request.type());
  if (stride == 0) {
    return std::unexpected("the field type is unknown");
  }
  auto address = FieldAddress(*entity, request.class_name(), request.field(), sizeof(ListHeader));
  if (!address) {
    return std::unexpected(address.error());
  }
  auto list = ReadList(*address);
  if (!list) {
    return std::unexpected(list.error());
  }

  // Copy every element through the operating system, then decode each.
  std::vector<unsigned char> bytes(static_cast<size_t>(list->size) * stride);
  if (!bytes.empty() && !gameinterop::ReadNative(list->elements, bytes.data(), bytes.size())) {
    return std::unexpected("cannot read " + request.class_name() + "." + request.field());
  }
  FieldListResponse response;
  for (size_t at = 0; at < bytes.size(); at += stride) {
    auto value = Decode(request.type(), &bytes[at]);
    if (!value) {
      return std::unexpected(value.error());
    }
    *response.add_values() = std::move(*value);
  }
  return response;
}

std::expected<void, std::string> Game::WriteFieldList(const WriteFieldListRequest& request) {
  // Find the entity and encode every value.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  const auto stride = Size(request.type());
  if (stride == 0) {
    return std::unexpected("the field type is unknown");
  }
  if (request.values_size() > kMaxList) {
    return std::unexpected("the list is too long");
  }
  std::vector<unsigned char> bytes;
  for (const auto& value : request.values()) {
    auto encoded = Encode(request.type(), value);
    if (!encoded) {
      return std::unexpected(encoded.error());
    }
    bytes.insert(bytes.end(), encoded->begin(), encoded->begin() + stride);
  }

  // Find the list field's address and its header.
  auto address = FieldAddress(*entity, request.class_name(), request.field(), sizeof(ListHeader));
  if (!address) {
    return std::unexpected(address.error());
  }
  auto list = ReadList(*address);
  if (!list) {
    return std::unexpected(list.error());
  }

  // A list that is too short takes a larger buffer from the game's allocator,
  // which the game frees with the list. The old buffer is left to the game
  // since its owner is unknown, and doubling bounds what that leaves behind.
  const auto count = request.values_size();
  if (count > list->capacity) {
    const int32_t capacity = std::clamp(2 * list->capacity, count, kMaxList);
    void* grown = gameinterop::AllocateNative(static_cast<size_t>(capacity) * stride);
    if (grown == nullptr) {
      return std::unexpected("cannot grow " + request.field());
    }
    list->elements = grown;
    list->capacity = capacity;
  }

  // Fill the elements before the header, so the list never counts an element
  // that is not there.
  const std::string name = request.class_name() + "." + request.field();
  if (!bytes.empty() && !gameinterop::WriteNative(list->elements, bytes.data(), bytes.size())) {
    return std::unexpected("cannot write " + name);
  }
  list->size = count;
  if (!gameinterop::WriteNative(*address, &*list, sizeof(*list))) {
    return std::unexpected("cannot write " + name);
  }
  if (!gameinterop::NotifyEntityStateChanged(*entity)) {
    return std::unexpected("cannot replicate " + name);
  }
  return {};
}

std::expected<EntityClassResponse, std::string> Game::EntityClass(const EntityRequest& request) {
  // Find the entity and its schema class.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto schema = gameinterop::SchemaClassOfEntity(*entity, gameinterop::ReadNative);
  if (!schema) {
    return std::unexpected(schema.error());
  }

  // Answer the class and designer names.
  EntityClassResponse response;
  auto* name = response.mutable_name();
  name->set_class_name(std::string(gameinterop::SchemaClassNameOf(*schema)));
  name->set_designer_name(std::string(DesignerName(*entity)));
  return response;
}

std::expected<ActiveResponse, std::string> Game::ModifierState(
    const ModifierStateRequest& request) {
  // Find the entity and the mask word that holds the state.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto word = FindModifierWord(*entity, request.state());
  if (!word) {
    return std::unexpected(word.error());
  }
  ActiveResponse response;
  if (!*word) {
    return response;
  }

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
  // Releasing a hold clears the state the hold set.
  auto hold = std::pair(request.entity(), request.state());
  if (request.has_active() && !request.active()) {
    if (held_.erase(hold) == 0) {
      return {};
    }
    return SetModifierState(request.entity(), request.state(), false);
  }

  // Holding sets the state now and every frame after.
  if (auto set = SetModifierState(request.entity(), request.state(), true); !set) {
    return set;
  }
  held_.insert(std::move(hold));
  return {};
}

std::expected<void, std::string> Game::SetFreeForAll(const SetFreeForAllRequest& request) {
  if (free_for_all_ == request.enabled()) {
    return {};
  }
  free_for_all_ = request.enabled();
  if (!free_for_all_) {
    ReleaseFreeForAll();
  }
  return {};
}

bool Game::FreeForAllCommand(std::string_view line) const {
  // Only changeteam is blocked, and only in free-for-all.
  if (!free_for_all_) {
    return false;
  }
  const auto name = line.substr(0, line.find_first_of(" \t"));
  return name == "changeteam";
}

void Game::ApplyFreeForAll() {
  // Seat players and hold friendly fire only in free-for-all.
  if (!free_for_all_) {
    return;
  }
  static constexpr const char* kFriendlyFire = "MODIFIER_STATE_FRIENDLY_FIRE_ENABLED";
  auto selection = services_.Selection();
  if (selection) {
    // Seat each player with no team on the side with fewer players.
    std::array<int, 4> sides{};
    std::vector<int32_t> unseated;
    for (int32_t slot = 0; slot < kMaxPlayers; ++slot) {
      const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
      if (!state.occupied) {
        continue;
      }
      const auto team = observer_.ControllerTeam(slot);
      if (!team) {
        continue;
      }
      if (*team == 0 && !state.is_bot && state.xuid != 0) {
        unseated.push_back(slot);
      } else if (*team < sides.size()) {
        ++sides[*team];
      }
    }
    for (const auto slot : unseated) {
      const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
      const int team = sides[3] < sides[2] ? 3 : 2;
      auto seated = observer_.SeatForChoice(slot, state.xuid, state.generation, team, **selection);
      if (seated && *seated) {
        ++sides[team];
        ffa_failed_.erase(slot);
      } else if (!seated && ffa_failed_.insert(slot).second) {
        std::cerr << "free-for-all: " << seated.error() << '\n';
      }
    }
  } else if (ffa_failed_.insert(-1).second) {
    std::cerr << "free-for-all: " << selection.error() << '\n';
  }

  // Hold friendly fire on each living hero, and let a gone hero's hold go.
  std::set<uint32_t> living;
  for (const auto& hero : Heroes()) {
    living.insert(hero.handle);
    if (ffa_held_.insert(hero.handle).second) {
      held_.insert({hero.handle, kFriendlyFire});
    }
  }
  std::erase_if(ffa_held_, [&](uint32_t handle) {
    if (living.contains(handle)) {
      return false;
    }
    held_.erase({handle, kFriendlyFire});
    return true;
  });
}

void Game::ReleaseFreeForAll() {
  static constexpr const char* kFriendlyFire = "MODIFIER_STATE_FRIENDLY_FIRE_ENABLED";
  for (const auto handle : ffa_held_) {
    if (held_.erase({handle, kFriendlyFire}) != 0) {
      static_cast<void>(SetModifierState(handle, kFriendlyFire, false));
    }
  }
  ffa_held_.clear();
  ffa_failed_.clear();
}

std::expected<std::optional<Game::ModifierWord>, std::string> Game::FindModifierWord(
    void* entity, const std::string& state) {
  // Resolve the mask layout and the state's bit.
  auto layout = services_.Modifiers();
  if (!layout) {
    return std::unexpected(layout.error());
  }
  auto value = services_.ModifierState(state);
  if (!value) {
    return std::unexpected(value.error());
  }
  if (*value < 0 || static_cast<size_t>(*value) >= layout->mask_size * 8) {
    return std::unexpected(state + " is outside the modifier state masks");
  }

  // Read the entity's modifier property.
  unsigned char* property = nullptr;
  if (!gameinterop::ReadNative(static_cast<const unsigned char*>(entity) + layout->property,
                               &property, sizeof(property))) {
    return std::unexpected("cannot read the entity's modifiers");
  }
  if (property == nullptr) {
    return std::nullopt;
  }

  // Point at the word that holds the state in each mask.
  const auto word = static_cast<size_t>(*value / 32) * sizeof(uint32_t);
  return ModifierWord{
      .ordinary = property + layout->mask + word,
      .predicted = property + layout->predicted_mask + word,
      .bit = uint32_t{1} << (*value % 32),
  };
}

std::expected<void, std::string> Game::SetModifierState(uint32_t handle, const std::string& state,
                                                        bool active) {
  // Find the entity and the mask word that holds the state.
  auto entity = Entity(handle);
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto word = FindModifierWord(*entity, state);
  if (!word) {
    return std::unexpected(word.error());
  }
  if (!*word) {
    return std::unexpected("the entity has no modifiers");
  }

  // Read the ordinary mask and set or clear the state's bit.
  uint32_t current = 0;
  if (!gameinterop::ReadNative((*word)->ordinary, &current, sizeof(current))) {
    return std::unexpected("cannot read the entity's modifier states");
  }
  const uint32_t desired = active ? current | (*word)->bit : current & ~(*word)->bit;
  if (desired == current) {
    return {};
  }

  // Write the mask back and send it.
  if (!gameinterop::WriteNative((*word)->ordinary, &desired, sizeof(desired))) {
    return std::unexpected("cannot write the entity's modifier states");
  }
  if (!gameinterop::NotifyEntityStateChanged(*entity)) {
    return std::unexpected("cannot replicate the entity's modifier states");
  }
  return {};
}

std::expected<HeroResponse, std::string> Game::SelectHero(const SelectHeroRequest& request) {
  // Resolve the hero and the selection service.
  auto heroes = services_.Heroes();
  if (!heroes) {
    return std::unexpected(heroes.error());
  }
  auto hero = FindHero(**heroes, request);
  if (!hero) {
    return std::unexpected(hero.error());
  }
  auto selection = services_.Selection();
  if (!selection) {
    return std::unexpected(selection.error());
  }

  // Select the hero for the player in that slot.
  const auto state = gameinterop::ConnectionTracker::StateForSlot(request.player());
  if (!state.occupied) {
    return std::unexpected("no player is in that slot");
  }
  auto selected =
      observer_.SelectPlayer(request.player(), state.xuid, state.generation, request.team(),
                             hero->native_definition_pointer, **selection);
  if (!selected) {
    return std::unexpected(selected.error());
  }

  // Answer the hero selected.
  HeroResponse response;
  response.set_hero(static_cast<uint32_t>(hero->id));
  return response;
}

std::expected<void, std::string> Game::Spectate(const PlayerRequest& request) {
  // Resolve the selection service and the player in that slot.
  auto selection = services_.Selection();
  if (!selection) {
    return std::unexpected(selection.error());
  }
  const auto state = gameinterop::ConnectionTracker::StateForSlot(request.player());
  if (!state.occupied) {
    return std::unexpected("no player is in that slot");
  }

  return observer_.SelectPlayer(request.player(), state.xuid, state.generation, 1, nullptr,
                                **selection);
}

std::expected<void, std::string> Game::Watch(const WatchRequest& request) {
  // Find the camera's observer services, which aim it.
  if (auto target = Entity(request.target()); !target) {
    return std::unexpected(target.error());
  }

  // Find the camera pawn of the player's slot.
  auto selection = services_.Selection();
  if (!selection) {
    return std::unexpected(selection.error());
  }
  auto pawn = observer_.CameraPawnForSlot(request.player(), **selection);
  if (!pawn) {
    return std::unexpected(pawn.error());
  }

  // Read the pawn's observer services.
  auto services = services_.Field("CBasePlayerPawn", "m_pObserverServices");
  if (!services) {
    return std::unexpected(services.error());
  }
  unsigned char* observer = nullptr;
  if (!gameinterop::ReadNative(static_cast<unsigned char*>(*pawn) + services->offset, &observer,
                               sizeof(observer)) ||
      observer == nullptr) {
    return std::unexpected("the player's camera has no observer services");
  }

  // Resolve the observer target and mode fields.
  auto target_field = services_.Field("CPlayer_ObserverServices", "m_hObserverTarget");
  if (!target_field) {
    return std::unexpected(target_field.error());
  }
  auto mode_field = services_.Field("CPlayer_ObserverServices", "m_iObserverMode");
  if (!mode_field) {
    return std::unexpected(mode_field.error());
  }

  // Follow the target in its own view, OBS_MODE_IN_EYE, and replicate it.
  const uint32_t handle = request.target();
  const uint8_t in_eye = 2;
  if (!gameinterop::WriteNative(observer + target_field->offset, &handle, sizeof(handle)) ||
      !gameinterop::WriteNative(observer + mode_field->offset, &in_eye, sizeof(in_eye))) {
    return std::unexpected("cannot aim the spectator's camera");
  }
  if (!gameinterop::NotifyEntityStateChanged(*pawn)) {
    return std::unexpected("cannot replicate the spectator's camera");
  }
  return {};
}

std::expected<void, std::string> Game::Unwatch(const PlayerRequest& request) {
  // Return the player's camera to their hero.
  auto selection = services_.Selection();
  if (!selection) {
    return std::unexpected(selection.error());
  }
  auto camera = observer_.ReturnToHero(request.player(), **selection);
  if (!camera) {
    return std::unexpected(camera.error());
  }
  if (!*camera) {
    return {};
  }

  // Remove the camera entity the player watched through.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  static_cast<void>((*world)->RemoveEntity(*camera));
  return {};
}

std::expected<void, std::string> Game::Respawn(const PlayerRequest& request) {
  // Resolve the respawn call and the player's live pawn.
  auto respawn = services_.Respawn();
  if (!respawn) {
    return std::unexpected(respawn.error());
  }
  auto sample = Live(request.player());
  if (!sample) {
    return std::unexpected(sample.error());
  }
  return observer_.RespawnPlayer(request.player(), sample->session_generation, *respawn);
}

std::expected<void, std::string> Game::ClearItems(const PlayerRequest& request) {
  // Resolve the item tables and the player's live pawn.
  auto abilities = services_.Abilities();
  if (!abilities) {
    return std::unexpected(abilities.error());
  }
  auto items = services_.Items();
  if (!items) {
    return std::unexpected(items.error());
  }
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }
  return observer_.ReconcileItems(request.player(), {}, **abilities, **items);
}

std::expected<void, std::string> Game::Freeze(const FreezeRequest& request) {
  // Check the player's hero is live, then freeze or release it.
  const auto slot = request.player();
  const bool frozen = !request.has_frozen() || request.frozen();
  if (auto sample = Live(slot); !sample) {
    return std::unexpected(sample.error());
  }
  if (auto done = observer_.SetPreparationFrozen(slot, frozen); !done) {
    return done;
  }

  // Remember the frozen heroes, so Clear can release them.
  if (frozen) {
    frozen_.insert(slot);
  } else {
    frozen_.erase(slot);
  }
  return {};
}

std::expected<void, std::string> Game::RestoreStamina(const PlayerRequest& request) {
  auto now = LatchTime();
  if (!now) {
    return std::unexpected(now.error());
  }
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }
  return observer_.SetPracticeStamina(request.player(), *now);
}

std::expected<void, std::string> Game::SetStamina(const SetStaminaRequest& request) {
  auto now = LatchTime();
  if (!now) {
    return std::unexpected(now.error());
  }
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }
  return observer_.SetPracticeStamina(request.player(), *now, request.stamina());
}

std::expected<float, std::string> Game::LatchTime() {
  // The simulation time keeps running through a pause, so a latch written
  // with it after a pause sits in the future of a clock without the paused
  // ticks, and the hero shows empty stamina until that clock catches up. A
  // latch without the paused ticks fills the hero under either clock. Without
  // game rules no pause has happened, and the two clocks agree.
  auto server = services_.Server();
  if (!server) {
    return std::unexpected(server.error());
  }

  // Read the server clock.
  auto now = (*server)->ReadClock();
  if (!now) {
    return std::unexpected(now.error());
  }

  // Without a match clock, the server time is the game time.
  auto clock = services_.Clock();
  if (!clock) {
    return now->current_time;
  }
  return (*clock)->GameTime(now->current_time, now->interval).value_or(now->current_time);
}

std::expected<void, std::string> Game::RefreshAbility(const RefreshAbilityRequest& request) {
  // Find the player's ability and clear its cooldown.
  auto ability = OwnedAbility(request.player(), request.ability());
  if (!ability) {
    return std::unexpected(ability.error());
  }
  ability->cooldown_start = 0;
  ability->cooldown_end = 0;
  return observer_.ApplyAbilityTimers(request.player(), std::span(&*ability, 1));
}

std::expected<AbilitiesResponse, std::string> Game::Abilities(const PlayerRequest& request) {
  // Resolve the ability table and the player's live abilities.
  const auto slot = request.player();
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  if (auto sample = Live(slot); !sample) {
    return std::unexpected(sample.error());
  }
  auto abilities = observer_.CurrentAbilitiesForSlot(slot);
  if (!abilities) {
    return std::unexpected(abilities.error());
  }

  // Answer each ability's name, slot and timers.
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
  // Find the player's ability.
  auto ability = OwnedAbility(request.player(), request.ability());
  if (!ability) {
    return std::unexpected(ability.error());
  }

  // Apply the requested upgrades.
  if (request.has_upgrades()) {
    auto items = services_.Items();
    if (!items) {
      return std::unexpected(items.error());
    }
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

  // Limit the ability's charges.
  if (request.has_max_charges()) {
    if (auto limited = LimitCharges(request.player(), ability->handle, request.max_charges());
        !limited) {
      return limited;
    }
  }

  // Stop when no timer was asked for.
  if (!request.has_charges() && !request.has_cooldown_end() && !request.has_recharge_end()) {
    return {};
  }

  // An upgrade can change the charges, so set them on the upgraded ability.
  ability = OwnedAbility(request.player(), request.ability());
  if (!ability) {
    return std::unexpected(ability.error());
  }
  if (request.has_charges()) {
    ability->charges = request.charges();
  }
  if (request.has_cooldown_end()) {
    ability->cooldown_end = request.cooldown_end();
  }
  if (request.has_recharge_end()) {
    ability->charge_recharge_end = request.recharge_end();
  }
  return observer_.ApplyAbilityTimers(request.player(), std::span(&*ability, 1));
}

std::expected<void, std::string> Game::LimitCharges(int32_t slot, uint32_t handle,
                                                    int32_t maximum) {
  // Create the charge limit once from the server module.
  if (!charge_limit_) {
    auto image = services_.Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto slots = gameinterop::ResolveAbilityChargeSlots(**image);
    if (!slots) {
      return std::unexpected(slots.error());
    }
    auto created = gameinterop::AbilityChargeLimit::Create(slots->max_charges);
    if (!created) {
      return std::unexpected(created.error());
    }
    charge_limit_ = std::move(*created);
  }

  // Limit the live ability.
  auto* ability = observer_.AbilityInstance(slot, handle);
  if (ability == nullptr) {
    return std::unexpected("the ability is not live");
  }
  return charge_limit_->Set(ability, handle, maximum);
}

std::expected<void, std::string> Game::GiveItem(const GiveItemRequest& request) {
  // Resolve the item tables and the player's live pawn.
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  auto items = services_.Items();
  if (!items) {
    return std::unexpected(items.error());
  }
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }

  // Grant the item to the player's hero.
  auto item = observer_.GrantItem(
      request.player(), gameinterop::MakeMemberName(request.item()).hash, **definitions, **items);
  if (!item) {
    return std::unexpected(item.error());
  }
  return {};
}

std::expected<void, std::string> Game::ReplaceAbility(const ReplaceAbilityRequest& request) {
  // Check the slot and resolve the ability tables.
  if (request.index() > UINT16_MAX) {
    return std::unexpected("the ability slot is out of range");
  }
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  auto slots = services_.Slots();
  if (!slots) {
    return std::unexpected(slots.error());
  }
  auto create = services_.CreateAbility();
  if (!create) {
    return std::unexpected(create.error());
  }

  // Replace the ability on the player's live hero.
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }
  return (*slots)->Replace(observer_, request.player(), static_cast<uint16_t>(request.index()),
                           gameinterop::MakeMemberName(request.ability()).hash, **definitions,
                           *create);
}

std::expected<void, std::string> Game::HoldModifier(const HoldModifierRequest& request) {
  // Releasing a hold drops the slot's claim on the modifier.
  const auto& key = request.modifier();
  if (request.has_active() && !request.active()) {
    ReleaseModifier(request.player(), key);
    return {};
  }

  // A held modifier is named by its ability and its own name.
  if (key.find('/') == std::string::npos) {
    return std::unexpected("name the modifier as <ability>/<modifier>");
  }

  // Resolve the modifier once for every slot that holds it.
  if (!modifiers_.contains(key)) {
    auto image = services_.Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto schema = services_.Schema();
    if (!schema) {
      return std::unexpected(schema.error());
    }
    auto modifier = gameinterop::AbilityModifier::Resolve(**image, *schema, key, -1);
    if (!modifier) {
      return std::unexpected(modifier.error());
    }
    modifiers_.emplace(key, *modifier);
  }
  held_modifiers_.emplace(request.player(), key);

  // A hold that cannot start now ends, so the mod learns why.
  auto renewed = RenewModifier(request.player(), key);
  if (!renewed) {
    ReleaseModifier(request.player(), key);
  }
  return renewed;
}

std::expected<void, std::string> Game::GiveModifier(const GiveModifierRequest& request) {
  // Split the modifier's name at its ability.
  const auto& key = request.modifier();
  const auto slash = key.find('/');
  if (slash == std::string::npos) {
    return std::unexpected("name the modifier as <ability>/<modifier>");
  }

  // Resolve the server module, the schema and the hero's abilities.
  auto image = services_.Image();
  if (!image) {
    return std::unexpected(image.error());
  }
  auto schema = services_.Schema();
  if (!schema) {
    return std::unexpected(schema.error());
  }
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }
  auto owned = observer_.CurrentAbilitiesForSlot(request.player());
  if (!owned) {
    return std::unexpected(owned.error());
  }

  // The modifier's own ability applies it when the hero owns that ability;
  // otherwise the hero's first ability does.
  auto source = gameinterop::MakeMemberName(std::string_view(key).substr(0, slash)).hash;
  if (std::ranges::none_of(*owned,
                           [&](const auto& ability) { return ability.subclass_id == source; })) {
    const auto first = std::ranges::find(*owned, 0, &gameinterop::PawnObserver::Ability::slot);
    if (first == owned->end()) {
      return std::unexpected("the hero has no ability to apply the modifier");
    }
    source = first->subclass_id;
  }

  // The modifier takes its duration when attached, so the shared definition
  // gets its own duration back right away.
  auto modifier = gameinterop::AbilityModifier::Resolve(**image, *schema, key, request.seconds());
  if (!modifier) {
    return std::unexpected(modifier.error());
  }
  auto attached = modifier->Attach(observer_, request.player(), source);
  modifier->Restore();
  return attached;
}

std::expected<gameinterop::PawnObserver::Ability, std::string> Game::OwnedAbility(
    int32_t slot, std::string_view name) {
  // Resolve the ability table and the player's live abilities.
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  if (auto sample = Live(slot); !sample) {
    return std::unexpected(sample.error());
  }
  auto abilities = observer_.CurrentAbilitiesForSlot(slot);
  if (!abilities) {
    return std::unexpected(abilities.error());
  }

  // Find the owned ability with that name.
  for (const auto& ability : *abilities) {
    const auto definition = (*definitions)->Find(ability.subclass_id);
    if (definition && definition->name == name) {
      return ability;
    }
  }
  return std::unexpected("the hero has no ability named " + std::string(name));
}

std::expected<void, std::string> Game::Teleport(const TeleportRequest& request) {
  // Resolve the teleport call and the player's live hero.
  auto teleport = services_.Teleport();
  if (!teleport) {
    return std::unexpected(teleport.error());
  }
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }

  // Teleport the hero, with a velocity when the request sets one.
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

std::expected<void, std::string> Game::MovePlayer(const MovePlayerRequest& request) {
  // Resolve the motion setters and the player's live hero.
  auto motion = services_.Motion();
  if (!motion) {
    return std::unexpected(motion.error());
  }
  const auto slot = request.player();
  if (auto sample = Live(slot); !sample) {
    return std::unexpected(sample.error());
  }

  // Convert the requested pose to the game's floats.
  const auto position = Floats(request.position());
  const auto facing = Floats(request.facing());
  const auto velocity = Floats(request.velocity());

  // The aim takes the whole facing; tilting the body by pitch would lean the
  // whole hero.
  auto* pawn = observer_.PawnForSlot(slot);
  if (auto aimed = observer_.SetEyeAngles(pawn, facing); !aimed) {
    return aimed;
  }
  const std::array<float, 3> body{0, facing[1], 0};
  (*motion)->set_angles(pawn, body.data());
  (*motion)->set_velocity(pawn, velocity.data());
  (*motion)->set_origin(pawn, position.data());
  return {};
}

std::expected<void, std::string> Game::Steer(const SteerRequest& request) {
  // Without steering, the slot's bot goes back to its own commands.
  const auto slot = request.player();
  if (!request.has_steering()) {
    steers_.erase(slot);
    return services_.Steps();
  }

  // Steer the live hero, and drop the steering if the movement hook fails.
  if (auto sample = Live(slot); !sample) {
    return std::unexpected(sample.error());
  }
  steers_.insert_or_assign(
      slot, Steered{.pawn = observer_.PawnForSlot(slot), .steering = request.steering()});
  auto steps = services_.Steps();
  if (!steps) {
    steers_.erase(slot);
  }
  return steps;
}

std::expected<ObjectResponse, std::string> Game::CreateModel(const ModelOptions& request) {
  // Resolve the effect factory.
  auto effects = services_.Effects();
  if (!effects) {
    return std::unexpected(effects.error());
  }

  // Create the model and keep it as one of the mod's objects.
  render::WorldModelSettings settings{
      .resource = request.resource(),
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

std::expected<ObjectResponse, std::string> Game::CreateText(const TextOptions& request) {
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
  };
  auto entity = (*text)->Create(request.text(), request.position(), request.facing(), style);
  if (!entity) {
    return std::unexpected(entity.error());
  }
  return Keep(std::move(*entity));
}

std::expected<void, std::string> Game::MoveObject(const MoveObjectRequest& request) {
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

std::expected<void, std::string> Game::SetText(const SetTextRequest& request) {
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

std::expected<void, std::string> Game::SetObjectHidden(const SetObjectHiddenRequest& request) {
  // Showing the object again drops it from the player's hidden set.
  if (!objects_.contains(request.object())) {
    return std::unexpected("no object has that identifier");
  }
  if (!request.hidden()) {
    auto hidden = hidden_.find(request.player());
    if (hidden == hidden_.end()) {
      return {};
    }
    hidden->second.erase(request.object());
    if (hidden->second.empty()) {
      hidden_.erase(hidden);
    }
    return {};
  }

  // The engine sends only what its transmit pass allows, so hiding needs the
  // shared transmit hook.
  if (auto transmit = services_.Transmit(); !transmit) {
    return transmit;
  }
  hidden_[request.player()].insert(request.object());
  return {};
}

std::expected<void, std::string> Game::RemoveObject(const ObjectRequest& request) {
  // Find the object.
  auto found = objects_.find(request.object());
  if (found == objects_.end()) {
    return std::unexpected("no object has that identifier");
  }

  // Remove it from the world and from every hidden set.
  std::visit([](auto& handle) { handle->Remove(); }, found->second);
  objects_.erase(found);
  for (auto& [slot, objects] : hidden_) {
    objects.erase(request.object());
  }
  return {};
}

std::expected<EntityResponse, std::string> Game::ObjectEntity(const ObjectRequest& request) {
  // Find the object and check it is a model.
  auto found = objects_.find(request.object());
  if (found == objects_.end()) {
    return std::unexpected("no object has that identifier");
  }
  auto* model = std::get_if<std::unique_ptr<render::WorldEffect>>(&found->second);
  if (model == nullptr) {
    return std::unexpected("the object is text, which has no entity");
  }

  // Answer the model's live entity.
  auto handle = (*model)->Handle();
  if (!handle) {
    return std::unexpected("the object is no longer live");
  }
  EntityResponse response;
  response.set_entity(*handle);
  return response;
}

std::expected<BotResponse, std::string> Game::AddBot(const BotOptions& request) {
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
  BotResponse response;
  response.set_player(*slot);
  return response;
}

std::expected<void, std::string> Game::RemoveBot(const PlayerRequest& request) {
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

gameinterop::WorldEntities::Prepare Game::WriteBeforeSpawn(
    const google::protobuf::RepeatedPtrField<FieldWrite>& fields) {
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

std::expected<EntityResponse, std::string> Game::CreateEntity(const EntityOptions& request) {
  // Resolve the world and the requested key values.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  auto key_values = KeyValues(request.key_values());
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
  EntityResponse response;
  response.set_entity(*entity);
  return response;
}

std::expected<AliveResponse, std::string> Game::FireInput(const FireInputRequest& request) {
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

std::expected<AliveResponse, std::string> Game::RemoveEntity(const EntityRequest& request) {
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
  audiences_.erase(request.entity());
  return Alive((*world)->RemoveEntity(request.entity()));
}

std::expected<void, std::string> Game::ShowEntityOnlyTo(const ShowEntityOnlyToRequest& request) {
  // Only the mod's own entities take an audience, so a mod cannot keep a
  // player's hero or the map's entities from a client.
  if (!created_.contains(request.entity())) {
    return std::unexpected("the mod did not create the entity");
  }
  uint64_t slots = 0;
  for (const auto slot : request.players()) {
    if (slot < 0 || slot >= 64) {
      return std::unexpected("player slot out of range");
    }
    slots |= uint64_t{1} << slot;
  }

  // The engine sends only what its transmit pass allows, so an audience
  // needs the shared transmit hook.
  if (auto transmit = services_.Transmit(); !transmit) {
    return transmit;
  }
  audiences_[request.entity()] = slots;
  return {};
}

std::expected<void, std::string> Game::ShowEntityToAll(const EntityRequest& request) {
  if (!created_.contains(request.entity())) {
    return std::unexpected("the mod did not create the entity");
  }
  audiences_.erase(request.entity());
  return {};
}

void Game::PruneAudiences() {
  if (audiences_.empty() || !world_) {
    return;
  }
  // The transmit pass withholds by entity index alone, so a gone entity's
  // audience would otherwise apply to the next entity at its index.
  std::erase_if(audiences_, [this](const auto& entry) {
    auto live = world_->Exists(entry.first);
    return live && !*live;
  });
}

std::expected<NpcResponse, std::string> Game::SpawnNpc(const NpcOptions& request) {
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
  NpcResponse response;
  response.set_npc(*entity);
  return response;
}

std::expected<NpcStateResponse, std::string> Game::ReadNpc(const NpcRequest& request) {
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
  NpcStateResponse response;
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

std::expected<AliveResponse, std::string> Game::MoveNpc(const MoveNpcRequest& request) {
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  return Alive((*world)->Move(request.npc(), Floats(request.position()), Floats(request.facing()),
                              Floats(request.velocity())));
}

std::expected<AliveResponse, std::string> Game::SetNpcHealth(const SetNpcHealthRequest& request) {
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  const int32_t max_health = request.has_max_health() ? request.max_health() : request.health();
  return Alive((*world)->SetHealth(request.npc(), request.health(), max_health));
}

std::expected<AliveResponse, std::string> Game::RemoveNpc(const NpcRequest& request) {
  EntityRequest entity;
  entity.set_entity(request.npc());
  return RemoveEntity(entity);
}

std::expected<PickupResponse, std::string> Game::CreatePickup(const CreatePickupRequest& request) {
  // Resolve the world.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }

  // A pickup reads its aura from its game data entry. The urn's entry shares
  // its designer name; a movement buff names the movement pickup entry.
  const bool urn = request.kind() == PICKUP_KIND_URN;
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
  PickupResponse response;
  response.set_pickup(*entity);
  return response;
}

std::expected<AliveResponse, std::string> Game::PickupPresent(const PickupRequest& request) {
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  return Alive((*world)->Exists(request.pickup()));
}

std::expected<AliveResponse, std::string> Game::RemovePickup(const PickupRequest& request) {
  EntityRequest entity;
  entity.set_entity(request.pickup());
  return RemoveEntity(entity);
}

std::expected<EntitiesResponse, std::string> Game::FindEntities(
    const FindEntitiesRequest& request) {
  // Find the entities of the class.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  auto found = (*world)->Find(request.class_name());
  if (!found) {
    return std::unexpected(found.error());
  }
  EntitiesResponse response;
  response.mutable_entities()->Add(found->begin(), found->end());
  return response;
}

std::expected<CountResponse, std::string> Game::RemoveEntities(
    const RemoveEntitiesRequest& request) {
  // Remove the world's entities of the class and count them.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  auto removed = (*world)->Remove(request.class_name());
  if (!removed) {
    return std::unexpected(removed.error());
  }
  CountResponse response;
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

std::expected<void, std::string> Game::Damage(const HitOptions& request) {
  // Resolve the damage service and the victim.
  auto damage = services_.Damage();
  if (!damage) {
    return std::unexpected(damage.error());
  }
  auto victim = Entity(request.victim());
  if (!victim) {
    return std::unexpected(victim.error());
  }

  // Only heroes and units take hits: the client crashes on a damage event for
  // the world, a prop or a building.
  auto victim_class = gameinterop::SchemaClassOfEntity(*victim, gameinterop::ReadNative);
  if (!victim_class) {
    return std::unexpected(victim_class.error());
  }
  if (!services_.Derives(*victim_class, "CBaseCombatCharacter")) {
    return std::unexpected("the victim is a " +
                           std::string(gameinterop::SchemaClassNameOf(*victim_class)) +
                           ", not a hero or unit");
  }

  // A hit nobody dealt is a hazard: it may kill and credits no one.
  if (request.attacker() == 0) {
    return (*damage)->Hazard(*victim, request.amount());
  }

  // Resolve who dealt the hit and with what.
  auto attacker = Entity(request.attacker());
  if (!attacker) {
    return std::unexpected(attacker.error());
  }
  auto inflictor = request.inflictor() == 0 ? attacker : Entity(request.inflictor());
  if (!inflictor) {
    return std::unexpected(inflictor.error());
  }
  const int32_t hit_group = request.has_hit_group() ? request.hit_group() : -1;
  void* ability = nullptr;
  if (request.has_ability()) {
    auto found = Entity(request.ability());
    if (!found) {
      return std::unexpected(found.error());
    }
    ability = *found;
  }
  return (*damage)->Hit(*victim, *inflictor, *attacker, ability, request.amount(), hit_group);
}

std::expected<void, std::string> Game::AdjustSouls(const AdjustSoulsRequest& request) {
  // Adjust the live player's souls.
  auto currency = services_.Currency();
  if (!currency) {
    return std::unexpected(currency.error());
  }
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }
  auto adjusted =
      observer_.AdjustSouls(request.player(), request.delta(), request.silent(), *currency);
  if (!adjusted) {
    return std::unexpected(adjusted.error());
  }
  return {};
}

std::expected<void, std::string> Game::StartingSouls(const StartingSoulsRequest& request) {
  // Set the live player's starting souls.
  auto currency = services_.Currency();
  if (!currency) {
    return std::unexpected(currency.error());
  }
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }
  auto prepared = observer_.PrepareStartingSouls(request.player(), request.souls(), *currency);
  if (!prepared) {
    return std::unexpected(prepared.error());
  }
  return {};
}

std::expected<void, std::string> Game::ResetHero(const ResetHeroRequest& request) {
  // Reset the live player's hero to fresh starting souls.
  auto currency = services_.Currency();
  if (!currency) {
    return std::unexpected(currency.error());
  }
  auto reset = services_.ResetHero();
  if (!reset) {
    return std::unexpected(reset.error());
  }
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }
  auto fresh = observer_.ResetStartingSouls(request.player(), request.souls(), *currency, *reset);
  if (!fresh) {
    return std::unexpected(fresh.error());
  }
  return {};
}

std::expected<void, std::string> Game::RestoreHero(const RestoreHeroRequest& request) {
  // Build the restore target from the request.
  const auto& source = request.target();
  RestoreTarget target{
      .position = Floats(source.position()),
      .angles = Floats(source.facing()),
      .fresh = source.fresh(),
      .health = source.health(),
      .max_health = source.max_health(),
  };
  if (source.has_level()) {
    target.level = source.level();
  }
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

  // Resolve the live pawn and the engine services.
  auto sample = Live(request.player());
  if (!sample) {
    return std::unexpected(sample.error());
  }
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  auto server = services_.Server();
  if (!server) {
    return std::unexpected(server.error());
  }

  // Bind the restore to the engine and start it.
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
  if (!restore) {
    return std::unexpected(restore.error());
  }
  restores_.insert_or_assign(request.player(), std::move(*restore));
  return {};
}

bool Game::ApplyRestore(const HeroRestore::Sample& sample, const RestoreTarget& target,
                        HeroRestore::Stage stage) {
  // Log a step that fails, and borrow the player's pawn.
  const auto log = [](std::string_view what, const std::string& error) {
    std::cerr << "[modlock] hero restore: " << what << ": " << error << "\n";
    return false;
  };
  const auto slot = sample.slot;
  void* pawn = observer_.PawnForSlot(slot);

  // The build stage restores the level, items, abilities and upgrade bonuses.
  if (stage == HeroRestore::Stage::kBuild) {
    if (target.level && !gameinterop::RestorePawnLevel(pawn, *target.level)) {
      return false;
    }
    auto definitions = services_.Abilities();
    if (!definitions) {
      return log("abilities", definitions.error());
    }
    auto items = services_.Items();
    if (!items) {
      return log("items", items.error());
    }
    if (target.items) {
      if (auto done = observer_.ReconcileItems(slot, *target.items, **definitions, **items);
          !done) {
        return log("items", done.error());
      }
    }
    if (!target.abilities.empty()) {
      auto create = services_.CreateAbility();
      if (!create) {
        return log("abilities", create.error());
      }
      if (auto done = observer_.ReconcileAbilities(slot, target.abilities, **definitions, *create,
                                                   (*items)->set_bits);
          !done) {
        return log("abilities", done.error());
      }
    }

    // Item and ability calls can replace the pawn, so borrow it again.
    if (!observer_.CurrentAbilitiesForSlot(slot)) {
      return false;
    }
    pawn = observer_.PawnForSlot(slot);
    if (!target.fresh && target.upgrade_bonuses) {
      if (auto done = gameinterop::RestorePawnUpgradeBonuses(pawn, *target.upgrade_bonuses);
          !done) {
        return log("upgrade bonuses", done.error());
      }
    }
    return true;
  }

  // The pawn stage restores health, and full stamina for a fresh hero.
  if (stage == HeroRestore::Stage::kPawn) {
    if (target.fresh) {
      if (!sample.effective_max_health || *sample.effective_max_health <= 0) {
        return false;
      }
      auto server = services_.Server();
      if (!server) {
        return log("stamina", server.error());
      }
      auto clock = (*server)->ReadClock();
      if (!clock) {
        return log("stamina", clock.error());
      }
      if (auto done = observer_.SetPracticeStamina(slot, clock->current_time); !done) {
        return log("stamina", done.error());
      }
      pawn = observer_.PawnForSlot(slot);
    }
    const auto health = target.fresh ? *sample.effective_max_health : target.health;
    const auto max_health = target.fresh ? sample.max_health : target.max_health;
    if (!gameinterop::RestorePawnHealth(pawn, health, max_health)) {
      return false;
    }
  }

  // A bot has no camera to turn; its hero faces the angles and its AI aims.
  auto teleport = services_.Teleport();
  if (!teleport) {
    return log("pose", teleport.error());
  }
  pawn = observer_.PawnForSlot(slot);
  if (pawn == nullptr) {
    return false;
  }
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
  if (restores_.empty()) {
    return;
  }
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
  // Describe each NPC to restore.
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

  // Restore the NPCs into the world over the coming frames.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
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
  if (!restore) {
    return std::unexpected(restore.error());
  }
  npc_restore_ = std::move(*restore);
  return {};
}

std::expected<void, std::string> Game::Pause(const PauseRequest& request) {
  return SetPaused(request.paused());
}

std::expected<void, std::string> Game::SetPaused(bool paused) {
  // Pause or resume the game, and remember it.
  auto pause = services_.Pause();
  if (!pause) {
    return std::unexpected(pause.error());
  }
  if (auto set = (*pause)->Set(paused); !set) {
    return set;
  }
  paused_ = paused;
  return {};
}

std::expected<MatchClockResponse, std::string> Game::MatchClock() {
  // Resolve the match clock and the server's time.
  auto clock = services_.Clock();
  if (!clock) {
    return std::unexpected(clock.error());
  }
  auto server = services_.Server();
  if (!server) {
    return std::unexpected(server.error());
  }
  auto now = (*server)->ReadClock();
  if (!now) {
    return std::unexpected(now.error());
  }

  // Read the match clock in seconds.
  auto seconds = (*clock)->Read(now->current_time, now->interval);
  if (!seconds) {
    return std::unexpected(seconds.error());
  }
  MatchClockResponse response;
  response.set_seconds(*seconds);
  return response;
}

std::expected<void, std::string> Game::HoldMatchClock(const HoldMatchClockRequest& request) {
  // No seconds releases the hold.
  if (!request.has_seconds()) {
    held_clock_.reset();
    return {};
  }

  // Hold the clock at the requested seconds.
  if (auto clock = services_.Clock(); !clock) {
    return std::unexpected(clock.error());
  }
  held_clock_ = request.seconds();
  HoldClock();
  return {};
}

void Game::HoldClock() {
  // Read the clock while a hold is set.
  if (!held_clock_) {
    return;
  }
  auto clock = services_.Clock();
  auto server = services_.Server();
  if (!clock || !server) {
    return;
  }
  auto now = (*server)->ReadClock();
  if (!now) {
    return;
  }

  // A hold the game refuses ends with a log line instead of every frame.
  auto held = (*clock)->Restore(*held_clock_, now->current_time, now->interval);
  if (held) {
    held = ReplicateClock();
  }
  if (!held) {
    std::cerr << "[modlock] match clock hold ended: " << held.error() << "\n";
    held_clock_.reset();
  }
}

std::expected<void, std::string> Game::ReplicateClock() {
  auto rules = Entity(rules_entity_);
  if (!rules) {
    auto world = World();
    if (!world) {
      return std::unexpected(world.error());
    }
    auto found = (*world)->Find("citadel_gamerules");
    if (!found) {
      return std::unexpected(found.error());
    }
    if (found->empty()) {
      return std::unexpected("the game has no rules entity");
    }
    rules_entity_ = found->front();
    rules = Entity(rules_entity_);
    if (!rules) {
      return std::unexpected(rules.error());
    }
  }
  if (!gameinterop::NotifyEntityStateChanged(*rules)) {
    return std::unexpected("match clock replication is unavailable");
  }
  return {};
}

std::expected<RiftResponse, std::string> Game::Rift() {
  // Read the rift's state.
  auto rift = services_.Rift();
  if (!rift) {
    return std::unexpected(rift.error());
  }
  auto state = (*rift)->Read();
  if (!state) {
    return std::unexpected(state.error());
  }

  // Copy the scoring state into the response.
  RiftResponse response;
  auto* out = response.mutable_state();
  out->set_scoring_team(state->scoring_team);
  out->set_scoring_time(state->scoring_time);
  out->set_cash_in_started(state->cash_in_started);

  // Copy the spawn timing.
  out->set_give_up_time(state->give_up_time);
  out->set_next_spawn(state->next_spawn);
  out->set_spawn_window(state->spawn_window);
  return response;
}

std::expected<void, std::string> Game::StartRift(const StartRiftRequest& request) {
  auto rift = services_.Rift();
  if (!rift) {
    return std::unexpected(rift.error());
  }
  return (*rift)->StartAt(Floats(request.position()));
}

std::expected<void, std::string> Game::MoveEntity(const MoveEntityRequest& request) {
  // Move the entity, keeping its facing and velocity unless given.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  const auto position = Floats(request.position());
  const auto facing = Floats(request.facing());
  const auto velocity = Floats(request.velocity());
  gameinterop::TeleportEntity(*entity, position.data(),
                              request.has_facing() ? facing.data() : nullptr,
                              request.has_velocity() ? velocity.data() : nullptr);
  return {};
}

std::expected<void, std::string> Game::EmitSound(const EmitSoundRequest& request) {
  // Play the sound on the entity.
  auto sound = services_.Sound();
  if (!sound) {
    return std::unexpected(sound.error());
  }
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  if (!(*sound)->Emit(*entity, request.sound().c_str())) {
    return std::unexpected("the game did not play " + request.sound());
  }
  return {};
}

std::expected<void, std::string> Game::Kill(const PlayerRequest& request) {
  // Kill the live player's hero unless it is already dead.
  auto damage = services_.Damage();
  if (!damage) {
    return std::unexpected(damage.error());
  }
  auto sample = Live(request.player());
  if (!sample) {
    return std::unexpected(sample.error());
  }
  if (sample->health <= 0) {
    return {};
  }
  return (*damage)->Kill(observer_.PawnForSlot(request.player()));
}

std::expected<void, std::string> Game::SetVelocity(const SetVelocityRequest& request) {
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }
  gameinterop::SetEntityVelocity(observer_.PawnForSlot(request.player()),
                                 Floats(request.velocity()));
  return {};
}

std::expected<ButtonsResponse, std::string> Game::Buttons(const PlayerRequest& request) {
  // Resolve the controller's pawn fields.
  auto pawn_field = services_.Field("CBasePlayerController", "m_hPawn");
  if (!pawn_field) {
    return std::unexpected(pawn_field.error());
  }
  auto hero_field = services_.Field("CCitadelPlayerController", "m_hHeroPawn");
  if (!hero_field) {
    return std::unexpected(hero_field.error());
  }

  // Resolve the button fields and the live entity system.
  auto movement = services_.Field("CBasePlayerPawn", "m_pMovementServices");
  if (!movement) {
    return std::unexpected(movement.error());
  }
  auto buttons = services_.Field("CPlayer_MovementServices", "m_nButtons");
  if (!buttons) {
    return std::unexpected(buttons.error());
  }
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) {
    return std::unexpected(entities.error());
  }

  // A player's controller is the entity at index slot + 1. A dead player's
  // input reaches the pawn that observes, so both pawns are read.
  const auto all = gameinterop::EntityInstances(*entities);
  const auto controller = std::ranges::find_if(all, [&](void* entity) {
    const auto handle = gameinterop::ReferenceHandleOf(entity);
    return handle && static_cast<int32_t>(*handle & 0x7fff) == request.player() + 1;
  });
  if (controller == all.end()) {
    return std::unexpected("the player is not connected");
  }
  ButtonsResponse response;
  for (const auto& field : {*pawn_field, *hero_field}) {
    uint32_t handle = 0;
    std::memcpy(&handle, static_cast<unsigned char*>(*controller) + field.offset, sizeof(handle));
    auto* pawn = static_cast<unsigned char*>(gameinterop::EntityInstance(*entities, handle));
    if (pawn == nullptr) {
      continue;
    }
    unsigned char* services = nullptr;
    std::memcpy(&services, pawn + movement->offset, sizeof(services));
    if (services == nullptr) {
      continue;
    }
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
  // A sample without its command still serves, so a missing hook is logged.
  if (auto steps = services_.Steps(); !steps) {
    std::cerr << "mod movement: " << steps.error() << '\n';
  }
  return {};
}

std::vector<MovementSample> Game::Movement(std::span<const MovementFact> facts) {
  std::vector<MovementSample> samples;
  for (const auto slot : movers_) {
    const auto sample = observer_.Observe(slot);
    if (!sample || sample->health <= 0 || !sample->movement) {
      continue;
    }
    const auto& movement = *sample->movement;
    if (!movement.abs_velocity || !movement.grounded_by_handle) {
      continue;
    }
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
    if (auto command = services_.Command(observer_.PawnForSlot(slot))) {
      *out.mutable_command() = std::move(*command);
    }
    if (movement.last_time_on_zipline) {
      out.set_zipline_time(*movement.last_time_on_zipline);
    }
    for (const auto& fact : facts) {
      if (fact.pawn != sample->pawn_handle) {
        continue;
      }
      out.add_actions(fact.action);
      if (fact.action == MOVEMENT_ACTION_ABILITY_EXECUTED) {
        out.add_casts(fact.ability);
      }
    }
  }
  return samples;
}

std::expected<void, std::string> Game::WatchProjectiles(const ProjectileOptions& request) {
  projectiles_ = request;
  if (!projectiles_.names().empty()) {
    services_.Projectiles();
  }
  return {};
}

std::vector<Game::Hero> Game::Heroes() {
  std::vector<Hero> heroes;
  for (int32_t slot = 0; slot < kMaxPlayers; ++slot) {
    const auto sample = observer_.Observe(slot);
    if (!sample || sample->health <= 0) {
      continue;
    }
    Hero hero{.slot = slot,
              .handle = sample->pawn_handle,
              .pawn = observer_.PawnForSlot(slot),
              .feet = {float(sample->x), float(sample->y), float(sample->z)}};
    if (sample->movement) {
      hero.velocity = sample->movement->abs_velocity;
    }
    heroes.push_back(hero);
  }
  return heroes;
}

void Game::TrackProjectiles() {
  // With no projectile watched, forget the ones seen.
  if (projectiles_.names().empty()) {
    projectiles_seen_.clear();
    return;
  }

  // Note each watched projectile and report the new ones as launches.
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) {
    return;
  }
  std::set<uint32_t> seen;
  for (void* entity : gameinterop::EntityInstances(*entities)) {
    const auto name = DesignerName(entity);
    if (!Watches(projectiles_, name)) {
      continue;
    }
    const auto handle = gameinterop::ReferenceHandleOf(entity);
    if (!handle) {
      continue;
    }
    seen.insert(*handle);
    if (projectiles_seen_.contains(*handle) || launches_.size() >= kMaxChanges) {
      continue;
    }
    LaunchEvent launch;
    launch.set_entity(*handle);
    launch.set_name(std::string(name));
    const auto [owner, player] = Owner(entity);
    launch.set_owner(owner);
    launch.set_player(player);
    if (const auto origin = Origin(entity)) {
      SetVector(launch.mutable_position(), *origin);
    }
    launches_.push_back(std::move(launch));
  }
  projectiles_seen_ = std::move(seen);
}

std::optional<ImpactEvent> Game::Watching(void* entity, const gameinterop::TraceResult& contact) {
  // Only a watched projectile reports an impact.
  const auto name = DesignerName(entity);
  if (!Watches(projectiles_, name)) {
    return std::nullopt;
  }

  // Name the projectile and its owner.
  ImpactEvent event;
  event.set_entity(gameinterop::ReferenceHandleOf(entity).value_or(0));
  event.set_name(std::string(name));
  const auto [owner, player] = Owner(entity);
  event.set_owner(owner);
  event.set_player(player);

  // Add the contact's path and what it hit.
  SetVector(event.mutable_start(), contact.start);
  SetVector(event.mutable_end(), contact.end);
  SetVector(event.mutable_position(), contact.position);
  event.set_hit(contact.entity_handle.value_or(0));
  return event;
}

std::pair<uint32_t, int32_t> Game::Owner(void* entity) {
  // Read the entity's owner handle.
  auto field = services_.Field("CBaseEntity", "m_hOwnerEntity");
  if (!field) {
    return {0, -1};
  }
  uint32_t owner = 0;
  std::memcpy(&owner, static_cast<unsigned char*>(entity) + field->offset, sizeof(owner));
  if (owner == UINT32_MAX) {
    return {0, -1};
  }

  // Find the player whose pawn owns it.
  for (int32_t slot = 0; slot < kMaxPlayers; ++slot) {
    const auto sample = observer_.Observe(slot);
    if (sample && sample->pawn_handle == owner) {
      return {owner, slot};
    }
  }
  return {owner, -1};
}

std::optional<std::array<float, 3>> Game::Origin(void* entity) {
  // Resolve the scene node fields.
  auto body = services_.Field("CBaseEntity", "m_CBodyComponent");
  auto node = services_.Field("CBodyComponent", "m_pSceneNode");
  auto origin = services_.Field("CGameSceneNode", "m_vecAbsOrigin");
  if (!body || !node || !origin) {
    return std::nullopt;
  }

  // Follow the body component to its scene node.
  unsigned char* component = nullptr;
  std::memcpy(&component, static_cast<unsigned char*>(entity) + body->offset, sizeof(component));
  if (component == nullptr) {
    return std::nullopt;
  }
  unsigned char* scene = nullptr;
  std::memcpy(&scene, component + node->offset, sizeof(scene));
  if (scene == nullptr) {
    return std::nullopt;
  }

  // Read the node's absolute origin.
  std::array<float, 3> position{};
  std::memcpy(position.data(), scene + origin->offset, sizeof(position));
  return position;
}

std::expected<TraceResponse, std::string> Game::Trace(const TraceOptions& request) {
  // Resolve the world's collision.
  auto trace = engine_->Trace();
  if (!trace) {
    return std::unexpected(trace.error());
  }
  if (!(*trace)->IsReady()) {
    return std::unexpected("the world's collision is not ready");
  }

  // Set the layers to hit and to exclude.
  gameinterop::TraceOptions options;
  if (request.layers() != 0) {
    options.interacts_with = request.layers();
  }
  options.interacts_exclude = request.exclude();

  // The game's trace skips at most two entities.
  if (request.ignore_size() > static_cast<int>(options.ignored_entities.size())) {
    return std::unexpected("a trace ignores at most two entities");
  }
  std::ranges::copy(request.ignore(), options.ignored_entities.begin());

  // Trace the line.
  auto result = (*trace)->Query(Floats(request.start()), Floats(request.end()),
                                gameinterop::TraceLine{}, options);
  if (!result) {
    return std::unexpected(result.error());
  }

  // Report the hit, if any.
  TraceResponse response;
  if (!result->DidHit()) {
    return response;
  }
  auto* hit = response.mutable_hit();
  SetVector(hit->mutable_position(), result->position);
  SetVector(hit->mutable_normal(), result->normal);
  hit->set_start_solid(result->start_in_solid);
  hit->set_entity(result->entity_handle.value_or(0));
  return response;
}

std::expected<ObjectResponse, std::string> Game::CreateParticle(const ParticleOptions& request) {
  // Resolve the effects service.
  auto effects = services_.Effects();
  if (!effects) {
    return std::unexpected(effects.error());
  }

  // Describe the particle with its optional tint, data and parent.
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
  if (request.parent() != 0) {
    settings.parent = request.parent();
  }

  // Create the particle and keep it.
  auto particle = (*effects)->CreateParticle(settings);
  if (!particle) {
    return std::unexpected(particle.error());
  }
  return Keep(std::unique_ptr<render::WorldEffect>(std::move(*particle)));
}

std::expected<ObjectResponse, std::string> Game::CreateFog(const FogOptions& request) {
  // kDrawDistance reaches the far edges of the largest map.
  constexpr float kDrawDistance = 16384;
  auto effects = services_.Effects();
  if (!effects) {
    return std::unexpected(effects.error());
  }

  // Create the map's fog controller once.
  if (!fog_) {
    auto controller = (*effects)->CreateFogController(request.position(), kDrawDistance);
    if (!controller) {
      return std::unexpected(controller.error());
    }
    fog_ = std::move(*controller);
  }

  // Create the fog volume with the color's tint.
  const uint32_t color = request.color();
  const std::array<uint8_t, 3> tint{static_cast<uint8_t>(color >> 24),
                                    static_cast<uint8_t>(color >> 16),
                                    static_cast<uint8_t>(color >> 8)};
  auto volume = (*effects)->CreateFogVolume(request.position(), Floats(request.facing()),
                                            Floats(request.mins()), Floats(request.maxs()),
                                            request.strength(), tint);
  if (!volume) {
    return std::unexpected(volume.error());
  }
  return Keep(std::move(*volume));
}

std::expected<void, std::string> Game::Heal(const HealRequest& request) {
  // Read the live hero.
  auto sample = Live(request.player());
  if (!sample) {
    return std::unexpected(sample.error());
  }
  if (sample->health <= 0) {
    return std::unexpected("the player's hero is dead");
  }

  // Raise its health, at most to its maximum.
  const int32_t maximum = sample->effective_max_health.value_or(sample->max_health);
  const int32_t health = std::min(maximum, sample->health + request.amount());
  if (health <= sample->health) {
    return {};
  }
  if (!gameinterop::RestorePawnHealth(observer_.PawnForSlot(request.player()), health,
                                      sample->max_health)) {
    return std::unexpected("cannot heal the player's hero");
  }
  return {};
}

std::expected<void, std::string> Game::ScreenEffect(const ScreenEffectRequest& request) {
  // Resolve the messages, the screen's owner and the effect.
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  auto owner = ScreenOwner(request.player());
  if (!owner) {
    return std::unexpected(owner.error());
  }
  auto state = ScreenState(request.effect());
  if (!state) {
    return std::unexpected(state.error());
  }

  // Show the effect with its timing.
  const auto& timing = request.timing();
  return (*messages)->ScreenEffect(request.player(), *owner, *state,
                                   {.delay = timing.delay(),
                                    .fade_in = timing.fade_in(),
                                    .hold = timing.hold(),
                                    .fade_out = timing.fade_out()});
}

std::expected<void, std::string> Game::ClearScreenEffect(const ClearScreenEffectRequest& request) {
  // Resolve the messages, the screen's owner and the effect.
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  auto owner = ScreenOwner(request.player());
  if (!owner) {
    return std::unexpected(owner.error());
  }
  auto state = ScreenState(request.effect());
  if (!state) {
    return std::unexpected(state.error());
  }
  return (*messages)->ClearScreenEffect(request.player(), *owner, *state);
}

std::expected<int32_t, std::string> Game::ScreenOwner(int32_t slot) {
  // The client drops an effect unless its owner is a live entity, so the
  // effect belongs to the player's hero.
  auto sample = Live(slot);
  if (!sample) {
    return std::unexpected(sample.error());
  }
  return static_cast<int32_t>(sample->pawn_handle & 0x7fff);
}

std::expected<void, std::string> Game::Sound(const SoundRequest& request) {
  // Play the sound on the live player's hero.
  auto sound = services_.Sound();
  if (!sound) {
    return std::unexpected(sound.error());
  }
  if (auto sample = Live(request.player()); !sample) {
    return std::unexpected(sample.error());
  }
  if (!(*sound)->Emit(observer_.PawnForSlot(request.player()), request.sound().c_str())) {
    return std::unexpected("the game did not play " + request.sound());
  }
  return {};
}

std::expected<gameinterop::WorldEntities*, std::string> Game::World() {
  if (!world_) {
    auto image = services_.Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto schema = services_.Schema();
    if (!schema) {
      return std::unexpected(schema.error());
    }
    auto world = gameinterop::WorldEntities::Resolve(**image, *schema);
    if (!world) {
      return std::unexpected(world.error());
    }
    world_ = std::move(*world);
  }
  return &*world_;
}

std::expected<gameinterop::PawnObserver::Sample, std::string> Game::Live(int32_t slot) {
  auto sample = observer_.Observe(slot);
  if (!sample) {
    return std::unexpected("the player has no live hero");
  }
  return *sample;
}

std::expected<void*, std::string> Game::Entity(uint32_t handle) const {
  // Find the live entity with that handle.
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) {
    return std::unexpected(entities.error());
  }
  auto* entity = gameinterop::EntityInstance(*entities, handle);
  if (entity == nullptr) {
    return std::unexpected("no live entity has that handle");
  }
  return entity;
}

ObjectResponse Game::Keep(Object object) {
  // Store the object under the next id and return the id.
  const auto id = next_object_++;
  objects_.emplace(id, std::move(object));
  ObjectResponse response;
  response.set_object(id);
  return response;
}

}  // namespace modlock::wasm
