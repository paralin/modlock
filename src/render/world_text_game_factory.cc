#include "modlock/render/world_text_game_factory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "modlock/gameinterop/entity_abi.h"

namespace modlock::render {
namespace {

using gameinterop::IdentityOf;
using gameinterop::InteropTraceEnabled;

constexpr size_t kMaxTraceFailures = 32;

// PlaceEntity teleports a world-text entity to origin with angles and no motion.
void PlaceEntity(void* entity, const modlock::Vec3& origin, const modlock::EulerAngles& angles) {
  gameinterop::TeleportEntity(entity,
                              {static_cast<float>(origin.x()), static_cast<float>(origin.y()),
                               static_cast<float>(origin.z())},
                              {static_cast<float>(angles.pitch()), static_cast<float>(angles.yaw()),
                               static_cast<float>(angles.roll())},
                              {});
}

using CreateEntityByNameFn = void* (*)(void*, const char*, int);

bool WriteWorldTextMessage(void* entity, const char* message) {
  if (entity == nullptr || message == nullptr) {
    return false;
  }
  const auto schema = modlock::gameinterop::ResolveSchemaSystem();
  if (!schema) return false;
  const auto field = modlock::gameinterop::SchemaFieldOf(*schema, "server.dll", "CPointWorldText",
                                                         "m_messageText");
  if (!field) return false;
  constexpr size_t kMinimumInlineTextCapacity = 64;
  constexpr size_t kMaximumInlineTextCapacity = 4096;
  if (field->size < kMinimumInlineTextCapacity || field->size > kMaximumInlineTextCapacity) {
    return false;
  }
  auto* destination = static_cast<std::uint8_t*>(entity) + field->offset;
  std::memset(destination, 0, field->size);
  std::memcpy(destination, message, (std::min)(std::strlen(message), field->size - 1));
  return true;
}

}  // namespace

std::expected<std::unique_ptr<WorldTextGameFactory>, std::string> WorldTextGameFactory::TryCreate(
    const modlock::gameinterop::ModuleImage& server) {
  WorldTextGameCalls calls;
  struct Entry {
    std::string_view id;
    void** slot;
  };
  const Entry entries[] = {
      {"entity-system.create-entity-by-name",
       reinterpret_cast<void**>(&calls.create_entity_by_name)},
      {"entity-system.queue-spawn-entity", reinterpret_cast<void**>(&calls.queue_spawn_entity)},
      {"entity-system.execute-queued-creation",
       reinterpret_cast<void**>(&calls.execute_queued_creation)},
      {"entity-instance.accept-input", reinterpret_cast<void**>(&calls.accept_input)},
      {"entity.remove", reinterpret_cast<void**>(&calls.util_remove)},
  };
  for (const auto& entry : entries) {
    if (auto address = modlock::gameinterop::ResolveSignature(server, entry.id)) {
      *entry.slot = *address;
    } else {
      return std::unexpected(address.error());
    }
  }
  auto key_values = modlock::gameinterop::ResolveKeyValuesCalls(server);
  if (!key_values.has_value()) {
    return std::unexpected(key_values.error());
  }
  calls.key_values = *key_values;
  calls.write_message = &WriteWorldTextMessage;

  // Teleport is vtable slot 163 at call time.
  return std::unique_ptr<WorldTextGameFactory>(new WorldTextGameFactory(calls));
}

WorldTextGameFactory::WorldTextGameFactory(WorldTextGameCalls calls) : calls_(calls) {}

WorldTextGameFactory::~WorldTextGameFactory() = default;

// GameWorldTextEntity owns one live point_worldtext. SetMessage rebuilds the
// entity through the same proven creation chain; a
// failed rebuild keeps the previous text instead of dropping the handle.
class GameWorldTextEntity final : public WorldTextEntity {
 public:
  GameWorldTextEntity(WorldTextGameFactory& factory, std::string message,
                      const modlock::Vec3& origin, const modlock::EulerAngles& angles,
                      const WorldTextStyle& style)
      : factory_(factory),
        message_(std::move(message)),
        origin_(origin),
        angles_(angles),
        style_(style) {}

  ~GameWorldTextEntity() override { Remove(); }

  void SetMessage(std::string_view message) override {
    auto rebuilt = factory_.Create(message, origin_, angles_, style_);
    if (!rebuilt) {
      // Log-and-continue: keep the stale text rather than losing the label.
      return;
    }
    auto* replacement = static_cast<GameWorldTextEntity*>(rebuilt->get());
    rebuilt->release();
    Remove();
    entity_ = replacement->entity_;
    replacement->entity_ = nullptr;
    delete replacement;
    message_ = std::string(message);
  }

  void SetOrigin(const modlock::Vec3& position, const modlock::EulerAngles& angles) override {
    if (entity_ == nullptr) {
      return;
    }
    PlaceEntity(entity_, position, angles);
    origin_ = position;
    angles_ = angles;
  }

  std::optional<std::uint32_t> Handle() const override {
    return gameinterop::ReferenceHandleOf(entity_);
  }

  void Remove() override {
    if (entity_ == nullptr) {
      return;
    }
    factory_.calls_.util_remove(entity_);
    entity_ = nullptr;
  }

  void InvalidateAfterEngineReset() override {
    // The engine already destroyed this entity. Clearing the handle prevents
    // the destructor from calling UTIL_Remove on dead engine state.
    entity_ = nullptr;
    key_values_ = nullptr;
  }

  // Spawn creates the entity, applies keyvalues, then calls Teleport,
  // QueueSpawnEntity, ExecuteQueuedCreation. Returns an error naming the
  // failed game step.
  [[nodiscard]] std::expected<void, std::string> Spawn() {
    if (factory_.calls_.create_entity_by_name == nullptr) {
      return std::unexpected("world text: create-entity-by-name unresolved");
    }
    if (factory_.calls_.queue_spawn_entity == nullptr ||
        factory_.calls_.execute_queued_creation == nullptr ||
        factory_.calls_.accept_input == nullptr || factory_.calls_.write_message == nullptr ||
        factory_.calls_.entity_system == nullptr) {
      return std::unexpected(
          "world text: creation unresolved (QueueSpawnEntity, "
          "ExecuteQueuedCreation, AcceptInput, message writer, or entity system)");
    }
    const auto& key_values = factory_.calls_.key_values;
    const bool key_values_ready =
        key_values.create_key_values != nullptr ||
        (key_values.allocate != nullptr && key_values.construct_key_values != nullptr &&
         key_values.set_key_value != nullptr);
    if (!key_values_ready) {
      return std::unexpected("world text: keyvalues construction is unresolved");
    }
    void* entity = factory_.calls_.create_entity_by_name(nullptr, "point_worldtext", -1);
    if (entity == nullptr) {
      return std::unexpected("create point_worldtext returned null");
    }
    entity_ = entity;

    if (auto applied = ApplyKeyValues(); !applied) {
      Remove();
      return std::unexpected(applied.error());
    }

    PlaceEntity(entity_, origin_, angles_);

    factory_.calls_.queue_spawn_entity(factory_.calls_.entity_system, IdentityOf(entity_),
                                       key_values_);
    key_values_ = nullptr;
    factory_.calls_.execute_queued_creation(factory_.calls_.entity_system);

    if (!factory_.calls_.write_message(entity_, message_.c_str())) {
      return std::unexpected("point_worldtext message field was not writable after spawn");
    }
    gameinterop::Variant empty{.pointer = "", .type = gameinterop::VariantType::kCString};
    if (!factory_.calls_.accept_input(entity_, "Enable", nullptr, nullptr, &empty, 0, nullptr)) {
      return std::unexpected("point_worldtext rejected Enable after spawn");
    }
    return {};
  }

 private:
  // ApplyKeyValues sets enabled, fullbright text with the requested style.
  [[nodiscard]] std::expected<void, std::string> ApplyKeyValues() {
    const modlock::gameinterop::KeyValueColor color{
        .red = static_cast<std::uint8_t>(style_.color_abgr & 0xFF),
        .green = static_cast<std::uint8_t>((style_.color_abgr >> 8) & 0xFF),
        .blue = static_cast<std::uint8_t>((style_.color_abgr >> 16) & 0xFF),
        .alpha = static_cast<std::uint8_t>((style_.color_abgr >> 24) & 0xFF),
    };
    const modlock::gameinterop::EntityKeyValue values[] = {
        {.key = "message_text", .value = std::string_view(message_)},
        {.key = "enabled", .value = true},
        {.key = "fullbright", .value = 1},
        {.key = "font_name", .value = std::string_view("Reaver")},
        {.key = "font_size", .value = style_.font_size},
        {.key = "world_units_per_pixel",
         .value = (0.25f / 1050.0f) * style_.font_size * style_.scale},
        {.key = "color", .value = color},
        {.key = "justify_horizontal", .value = 0},
        {.key = "justify_vertical", .value = 0},
        {.key = "reorient_mode", .value = style_.face_camera ? 1 : 0},
    };
    auto built = modlock::gameinterop::BuildEntityKeyValues(factory_.calls_.key_values, values);
    if (!built.has_value()) {
      return std::unexpected("world text: " + built.error());
    }
    key_values_ = *built;
    return {};
  }

  WorldTextGameFactory& factory_;
  std::string message_;
  modlock::Vec3 origin_;
  modlock::EulerAngles angles_;
  WorldTextStyle style_;
  void* entity_ = nullptr;
  void* key_values_ = nullptr;
};

std::expected<std::unique_ptr<WorldTextEntity>, std::string> WorldTextGameFactory::Create(
    std::string_view message, const modlock::Vec3& origin, const modlock::EulerAngles& angles,
    const WorldTextStyle& style) {
  auto entity = std::unique_ptr<GameWorldTextEntity>(
      new GameWorldTextEntity(*this, std::string(message), origin, angles, style));
  if (auto spawned = entity->Spawn(); !spawned) {
    TraceFailure(spawned.error());
    return std::unexpected(spawned.error());
  }
  TraceSuccess();
  return entity;
}

void WorldTextGameFactory::TraceFailure(std::string_view message) {
  if (!InteropTraceEnabled() || trace_failures_.size() >= kMaxTraceFailures) {
    return;
  }
  if (!trace_failures_.emplace(message).second) {
    return;
  }
  std::fprintf(stderr, "[modlock] world-text create failed: %.*s\n",
               static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

void WorldTextGameFactory::TraceSuccess() {
  if (!InteropTraceEnabled() || trace_success_logged_) {
    return;
  }
  trace_success_logged_ = true;
  std::fprintf(stderr, "[modlock] world-text create succeeded: point_worldtext\n");
  std::fflush(stderr);
}

}  // namespace modlock::render
