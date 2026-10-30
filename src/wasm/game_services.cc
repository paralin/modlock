//go:build deps_only

#include <algorithm>
#include <cstring>
#include <iostream>
#include <type_traits>
#include <utility>

#include "modlock/gameinterop/console_variables.h"
#include "modlock/gameinterop/player_selection.h"
#include "wasm/console_lines.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

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
  // Refuse a line that reaches outside the game.
  if (auto refused = RefuseConsoleLine(command); !refused.empty()) {
    return std::unexpected(std::move(refused));
  }

  // Hold the line until the world starts.
  if (!started_) {
    pending_commands_.push_back(std::move(command));
    return {};
  }

  // Resolve the engine server that runs console lines.
  auto server = Server();
  if (!server) {
    return std::unexpected(server.error());
  }

  // A command that sets a variable may name a development-only or
  // cheat-protected one, which the console otherwise refuses. Exposing the
  // name of any other command finds no variable and changes nothing.
  if (auto variables = gameinterop::ConsoleVariables::Resolve()) {
    for (const auto& run : SplitConsoleLine(command)) {
      static_cast<void>(variables->AllowServerChanges(std::string(run.name).c_str()));
    }
  }

  // Run the line on the server console.
  if (!(*server)->ServerCommand((command + '\n').c_str())) {
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

namespace {

// Keep stores a resolved value in its cache, or passes the failure on.
template <typename Value, typename Resolved>
std::expected<void, std::string> Keep(Value& cache, Resolved resolved) {
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  cache = std::move(*resolved);
  return {};
}

}  // namespace

template <typename Value, typename Resolve>
std::expected<void, std::string> GameServices::Cache(Value& cache, Resolve resolve) {
  // A filled cache needs nothing more.
  if (cache) {
    return {};
  }

  // Resolve from the server image, with the schema when resolve takes it.
  auto image = Image();
  if (!image) {
    return std::unexpected(image.error());
  }
  if constexpr (std::is_invocable_v<Resolve, const gameinterop::ModuleImage&, void*>) {
    auto schema = Schema();
    if (!schema) {
      return std::unexpected(schema.error());
    }
    return Keep(cache, resolve(**image, *schema));
  } else {
    return Keep(cache, resolve(**image));
  }
}

std::expected<const gameinterop::HeroDefinitions*, std::string> GameServices::Heroes() {
  if (auto cached = Cache(heroes_, &gameinterop::HeroDefinitions::Resolve); !cached) {
    return std::unexpected(cached.error());
  }
  return &*heroes_;
}

std::expected<const gameinterop::WorldEntities::Calls*, std::string> GameServices::WorldCalls() {
  if (auto cached = Cache(world_calls_, &gameinterop::WorldEntities::ResolveCalls); !cached) {
    return std::unexpected(cached.error());
  }
  return &*world_calls_;
}

std::expected<const gameinterop::PlayerSelectionCalls*, std::string> GameServices::Selection() {
  if (auto cached = Cache(selection_, &gameinterop::PlayerSelectionCalls::Resolve); !cached) {
    return std::unexpected(cached.error());
  }
  return &*selection_;
}

std::expected<const gameinterop::AbilityDefinitions*, std::string> GameServices::Abilities() {
  if (auto cached = Cache(abilities_, &gameinterop::AbilityDefinitions::Resolve); !cached) {
    return std::unexpected(cached.error());
  }
  return &*abilities_;
}

std::expected<const gameinterop::ItemFunctions*, std::string> GameServices::Items() {
  if (auto cached = Cache(items_, &gameinterop::ItemFunctions::Resolve); !cached) {
    return std::unexpected(cached.error());
  }
  return &*items_;
}

std::expected<const gameinterop::AbilitySlots*, std::string> GameServices::Slots() {
  if (auto cached = Cache(slots_, &gameinterop::AbilitySlots::Resolve); !cached) {
    return std::unexpected(cached.error());
  }
  return &*slots_;
}

std::expected<gameinterop::CreateAbility, std::string> GameServices::CreateAbility() {
  if (auto cached = Cache(create_ability_, &gameinterop::ResolveCreateAbility); !cached) {
    return std::unexpected(cached.error());
  }
  return create_ability_;
}

std::expected<gameinterop::RespawnPawn, std::string> GameServices::Respawn() {
  if (auto cached = Cache(respawn_, &gameinterop::ResolveRespawnPawn); !cached) {
    return std::unexpected(cached.error());
  }
  return respawn_;
}

std::expected<gameinterop::ChangeTeamKeepingHero, std::string> GameServices::ChangeTeam() {
  if (auto cached = Cache(change_team_, &gameinterop::ResolveChangeTeamKeepingHero); !cached) {
    return std::unexpected(cached.error());
  }
  return change_team_;
}

std::expected<gameinterop::ResetHeroPawn, std::string> GameServices::ResetHero() {
  if (auto cached = Cache(reset_hero_, &gameinterop::ResolveResetHeroPawn); !cached) {
    return std::unexpected(cached.error());
  }
  return reset_hero_;
}

std::expected<gameinterop::TeleportClientCamera, std::string> GameServices::Teleport() {
  if (auto cached = Cache(teleport_, &gameinterop::ResolveTeleportClientCamera); !cached) {
    return std::unexpected(cached.error());
  }
  return teleport_;
}

std::expected<const gameinterop::PawnMotion*, std::string> GameServices::Motion() {
  if (auto cached = Cache(motion_, &gameinterop::ResolvePawnMotion); !cached) {
    return std::unexpected(cached.error());
  }
  return &*motion_;
}

std::expected<const gameinterop::EntityDynamics*, std::string> GameServices::Dynamics() {
  if (auto cached = Cache(dynamics_, &gameinterop::EntityDynamics::Resolve); !cached) {
    return std::unexpected(cached.error());
  }
  return &*dynamics_;
}

std::expected<const gameinterop::BotCreation*, std::string> GameServices::Bots() {
  if (!bots_) {
    // Resolve the bot calls once.
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

std::expected<gameinterop::FreeForAllRelations*, std::string> GameServices::Relations() {
  if (auto cached = Cache(relations_, &gameinterop::FreeForAllRelations::Install); !cached) {
    return std::unexpected(cached.error());
  }
  return &*relations_;
}

std::expected<render::WorldTextGameFactory*, std::string> GameServices::Text() {
  // Build the text factory once from the server module.
  if (auto cached = Cache(text_, &render::WorldTextGameFactory::TryCreate); !cached) {
    return std::unexpected(cached.error());
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
  if (auto cached = Cache(effects_, &render::WorldEffectGameFactory::TryCreate); !cached) {
    return std::unexpected(cached.error());
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
  if (auto cached = Cache(damage_, &gameinterop::NativeDamage::Resolve); !cached) {
    return std::unexpected(cached.error());
  }
  return &*damage_;
}

std::expected<gameinterop::ModifyCurrency, std::string> GameServices::Currency() {
  if (auto cached = Cache(currency_, &gameinterop::ResolveModifyCurrency); !cached) {
    return std::unexpected(cached.error());
  }
  return currency_;
}

std::expected<const gameinterop::NativeSound*, std::string> GameServices::Sound() {
  if (auto cached = Cache(sound_, &gameinterop::NativeSound::TryCreate); !cached) {
    return std::unexpected(cached.error());
  }
  return &*sound_;
}

std::expected<const gameinterop::GamePause*, std::string> GameServices::Pause() {
  if (auto cached = Cache(pause_, &gameinterop::GamePause::Resolve); !cached) {
    return std::unexpected(cached.error());
  }
  return &*pause_;
}

std::expected<gameinterop::MatchClock*, std::string> GameServices::Clock() {
  if (auto cached = Cache(clock_, &gameinterop::MatchClock::Resolve); !cached) {
    return std::unexpected(cached.error());
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
  if (auto cached = Cache(rift_, &gameinterop::KothRules::Resolve); !cached) {
    return std::unexpected(cached.error());
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

  // Resolve the modifier property, its list and its two state masks.
  auto property = Field("CBaseEntity", "m_pModifierProp");
  if (!property) {
    return std::unexpected(property.error());
  }
  auto list = Field("CModifierProperty", "m_vecModifiers");
  if (!list) {
    return std::unexpected(list.error());
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
      .list = list->offset,
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

}  // namespace modlock::wasm
