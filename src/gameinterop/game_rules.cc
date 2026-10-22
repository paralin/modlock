#include "modlock/gameinterop/game_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "modlock/gameinterop/entity_abi.h"

#if defined(_WIN32)
#include <windows.h>

#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

#if defined(_WIN32)

using HeroPrecacheFn = void(__fastcall*)(void* global_set, const char* hero_name,
                                         void* resource_ctx);

// Thunk state: one hook may exist at a time. The names and decoded targets
// are set once at install; the manifest context is per call.
std::vector<std::string> g_hero_names;
std::vector<std::string> g_resource_names;
void* g_precache_global = nullptr;
HeroPrecacheFn g_precache_fn = nullptr;
GameRulesHooks::AddResourceFn g_add_resource = nullptr;
safetyhook::InlineHook g_hook;

// Resource context the game's precache routine expects during a manifest
// build: {128-bit zero, manifest pointer}.
struct ResourceContext {
  std::int64_t zero_a;
  std::int64_t zero_b;
  void* manifest;
};

void PrecacheIntoCurrentManifest(void** original_args) {
  // Nothing is added without heroes or the precache pair.
  if (g_hero_names.empty()) {
    return;
  }
  if (g_precache_fn == nullptr || g_precache_global == nullptr) {
    std::cout << "[modlock] game rules: precache pair unresolved; manifest "
                 "carries no heroes\n";
    return;
  }

  // Precache each hero into the session manifest.
  ResourceContext ctx{0, 0, original_args != nullptr ? *original_args : nullptr};
  int count = 0;
  for (const auto& name : g_hero_names) {
    // An unknown name fails inside the game's own lookup without touching
    // the manifest; the log line keeps the outcome visible for the session.
    g_precache_fn(g_precache_global, name.c_str(), &ctx);
    ++count;
  }
  std::cout << "[modlock] game rules: precached " << count
            << " hero(es) into the session manifest\n";
}

__int64 __fastcall ManifestBuildThunk(void* thisptr, void** original_args) {
  // Build the manifest, then add the heroes and resources to it.
  const __int64 result = g_hook.call<__int64>(thisptr, original_args);
  PrecacheIntoCurrentManifest(original_args);
  void* manifest = original_args ? *original_args : nullptr;
  GameRulesHooks::RegisterResources(manifest, g_add_resource, g_resource_names);
  return result;
}

#endif

}  // namespace

struct GameRulesHooks::Impl {
  ~Impl() {
#if defined(_WIN32)
    // Remove the hook and forget what it registered.
    g_hook.reset();
    g_hero_names.clear();
    g_resource_names.clear();
    g_precache_global = nullptr;
    g_precache_fn = nullptr;
    g_add_resource = nullptr;
#endif
  }
};

GameRulesHooks::GameRulesHooks(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GameRulesHooks::GameRulesHooks(GameRulesHooks&&) noexcept = default;
GameRulesHooks& GameRulesHooks::operator=(GameRulesHooks&&) noexcept = default;
GameRulesHooks::~GameRulesHooks() = default;

bool GameRulesHooks::RegisterResources(void* manifest, AddResourceFn add_resource,
                                       std::span<const std::string> resources) {
  if (!manifest || !add_resource) {
    return false;
  }
  for (const auto& resource : resources) {
    add_resource(resource.c_str(), manifest);
  }
  return true;
}

void GameRulesHooks::AddPrecache(std::vector<std::string> heroes,
                                 std::vector<std::string> resources) {
#if defined(_WIN32)
  for (auto& hero : heroes) {
    if (std::find(g_hero_names.begin(), g_hero_names.end(), hero) == g_hero_names.end()) {
      g_hero_names.push_back(std::move(hero));
    }
  }
  for (auto& resource : resources) {
    if (std::find(g_resource_names.begin(), g_resource_names.end(), resource) ==
        g_resource_names.end()) {
      g_resource_names.push_back(std::move(resource));
    }
  }
#endif
}

std::expected<void* const*, std::string> ResolveCurrentGameRules(const ModuleImage& server) {
  // The force-KOTH callback loads the current rules global.
  auto global = ResolveSignature(server, "game-rules.current");
  if (!global) {
    return std::unexpected(global.error());
  }
  const auto address = reinterpret_cast<uintptr_t>(*global);
  const auto size = server.image_bytes().size();
  if (address < server.base() || size < sizeof(void*) ||
      address - server.base() > size - sizeof(void*)) {
    return std::unexpected("game-rules current pointer leaves server module");
  }
  return reinterpret_cast<void* const*>(address);
}

std::expected<GamePause, std::string> GamePause::Resolve(const ModuleImage& server,
                                                         void* schema_system) {
  // Find the current rules.
  auto current = ResolveCurrentGameRules(server);
  if (!current) {
    return std::unexpected(current.error());
  }

  // citadel_toggle_server_pause reads the current rules, toggles the replicated
  // server-pause field and runs the ordinary pause-modifier reconciliation.
  auto toggle = ResolveSignature(server, "game-rules.toggle-server-pause");
  if (!toggle) {
    return std::unexpected(toggle.error());
  }
  auto refresh = ResolveSignature(server, "game-rules.refresh-pause");
  if (!refresh) {
    return std::unexpected(refresh.error());
  }
  const auto refresh_address = reinterpret_cast<uintptr_t>(*refresh);
  if (refresh_address < server.base() ||
      refresh_address - server.base() >= server.image_bytes().size()) {
    return std::unexpected("native pause refresh leaves server module");
  }

  // The toggle's leading cmp reads m_bServerPaused; its displacement must
  // agree with the live schema before the field is read or the toggle runs.
  auto field = SchemaFieldOf(schema_system, "server.dll", "CCitadelGameRules", "m_bServerPaused");
  if (!field) {
    return std::unexpected(field.error());
  }
  constexpr size_t kPausedCmpDisplacement = 20;
  uint32_t compared_offset = 0;
  std::memcpy(&compared_offset,
              server.image_bytes().data() + (reinterpret_cast<uintptr_t>(*toggle) - server.base()) +
                  kPausedCmpDisplacement,
              sizeof(compared_offset));
  if (field->offset != compared_offset || field->size < sizeof(uint8_t)) {
    return std::unexpected("native server-pause field changed");
  }

  // Keep what was resolved.
  GamePause pause;
  pause.current_ = *current;
  pause.toggle_ = reinterpret_cast<void (*)()>(*toggle);
  pause.refresh_ = reinterpret_cast<void (*)(void*)>(*refresh);
  pause.paused_offset_ = field->offset;
  return pause;
}

std::expected<void, std::string> GamePause::Set(bool paused) const {
  // Read the active rules.
  void* rules = nullptr;
  if (current_) {
    std::memcpy(&rules, current_, sizeof(rules));
  }
  if (!rules || !toggle_ || !refresh_) {
    return std::unexpected("game-pause rules unavailable");
  }

  // Toggle when the state differs; refresh a pause that holds.
  auto* value = static_cast<const uint8_t*>(rules) + paused_offset_;
  if (*value > 1) {
    return std::unexpected("invalid native server-pause state");
  }
  if ((*value != 0) != paused) {
    toggle_();
  } else if (paused) {
    refresh_(rules);
  }

  // Read the state back.
  void* after = nullptr;
  std::memcpy(&after, current_, sizeof(after));
  if (after != rules || (*value != 0) != paused) {
    return std::unexpected("native game-pause readback differs");
  }
  return {};
}

std::expected<KothRules, std::string> KothRules::Resolve(const ModuleImage& server,
                                                         void* schema_system) {
  // Find the current rules and the server module's bounds.
  auto current = ResolveCurrentGameRules(server);
  if (!current) {
    return std::unexpected(current.error());
  }
  KothRules reader;
  reader.current_ = *current;
  reader.module_begin_ = server.base();
  reader.module_size_ = server.image_bytes().size();

  // The native force-spawn callback sets the next location and calls the
  // ordinary warning/spawn routine at +cb (E8, sole RCX argument is rules).
  auto start = ResolveSignature(server, "game-rules.start-koth");
  if (!start) {
    return std::unexpected(start.error());
  }
  const auto start_address = reinterpret_cast<uintptr_t>(*start);
  if (start_address < server.base() || start_address - server.base() >= reader.module_size_) {
    return std::unexpected("native KOTH start leaves server module");
  }
  reader.start_ = reinterpret_cast<void (*)(void*)>(*start);

  // Find where the next location is written.
  auto position =
      SchemaFieldOf(schema_system, "server.dll", "CCitadelGameRules", "m_vNextKothLocation");
  if (!position) {
    return std::unexpected(position.error());
  }
  if (position->size < sizeof(float) * 3) {
    return std::unexpected("KOTH location storage too short");
  }
  reader.next_position_offset_ = position->offset;

  // Find each timer field.
  constexpr const char* names[] = {"m_nKothScoringTeam",      "m_timeKothScoring",
                                   "m_timeKothCashInStarted", "m_timeKothGiveUp",
                                   "m_timeNextKothSpawn",     "m_timeNextKothSpawnWindowTime"};
  for (size_t i = 0; i < reader.offsets_.size(); ++i) {
    auto field = SchemaFieldOf(schema_system, "server.dll", "CCitadelGameRules", names[i]);
    if (!field) {
      return std::unexpected(field.error());
    }
    if (field->size < sizeof(uint32_t)) {
      return std::unexpected(std::string("KOTH field storage too short: ") + names[i]);
    }
    reader.offsets_[i] = field->offset;
  }
  return reader;
}

std::expected<void*, std::string> KothRules::Current() const {
  // Read the active rules and check their vtable.
  if (!current_) {
    return std::unexpected("native game rules reader is unresolved");
  }
  void* rules = nullptr;
  std::memcpy(&rules, current_, sizeof(rules));
  if (!rules) {
    return std::unexpected("native game rules are not active");
  }
  uintptr_t table = 0;
  std::memcpy(&table, rules, sizeof(table));
  if (table < module_begin_ || table - module_begin_ >= module_size_) {
    return std::unexpected("native game rules vtable is outside server module");
  }
  return rules;
}

std::expected<void, std::string> KothRules::StartAt(const std::array<float, 3>& position) {
  // Refuse a nonfinite location.
  for (float value : position) {
    if (!std::isfinite(value)) {
      return std::unexpected("KOTH location is nonfinite");
    }
  }

  // Read the rules and the objective's state.
  auto rules = Current();
  if (!rules) {
    return std::unexpected(rules.error());
  }
  if (!start_) {
    return std::unexpected("native KOTH start is unresolved");
  }
  const auto state = Read();
  if (!state) {
    return std::unexpected(state.error());
  }
  if (state->cash_in_started > 0 ||
      (started_after_scoring_ && state->scoring_time <= *started_after_scoring_)) {
    return std::unexpected("previous native KOTH objective has not ended");
  }

  // The native force-spawn callback writes this vector directly before the
  // same call; notification and objective creation remain native-owned.
  std::memcpy(static_cast<unsigned char*>(*rules) + next_position_offset_, position.data(),
              sizeof(float) * position.size());
  start_(*rules);
  started_after_scoring_ = state->scoring_time;
  return {};
}

std::expected<void, std::string> KothRules::Reset() {
  // Read the active rules.
  auto rules = Current();
  if (!rules) {
    return std::unexpected(rules.error());
  }

  // Zero every timer.
  auto* bytes = static_cast<unsigned char*>(*rules);
  const uint32_t zero = 0;
  for (const auto offset : offsets_) {
    std::memcpy(bytes + offset, &zero, sizeof(zero));
  }
  started_after_scoring_.reset();
  return {};
}

std::expected<KothState, std::string> KothRules::Read() const {
  // Read the active rules.
  auto rules = Current();
  if (!rules) {
    return std::unexpected(rules.error());
  }

  // Copy each field, refusing a nonfinite time.
  const auto* bytes = static_cast<const unsigned char*>(*rules);
  KothState state{};
  std::memcpy(&state.scoring_team, bytes + offsets_[0], sizeof(state.scoring_team));
  float* fields[] = {&state.scoring_time, &state.cash_in_started, &state.give_up_time,
                     &state.next_spawn, &state.spawn_window};
  for (size_t i = 0; i < std::size(fields); ++i) {
    std::memcpy(fields[i], bytes + offsets_[i + 1], sizeof(float));
    if (!std::isfinite(*fields[i])) {
      return std::unexpected("native KOTH time is nonfinite");
    }
  }
  return state;
}

std::expected<GameRulesHooks, std::string> GameRulesHooks::Install(
    const ModuleImage& server, std::vector<std::string> precache_heroes,
    std::vector<std::string> resources) {
#if defined(_WIN32)
  // Install only once.
  if (g_hook) {
    return std::unexpected("the session manifest hook is already installed");
  }

  // Stage 1: locate the manifest builder. Zero or multiple matches fail loud:
  // an engine update that shifts the pattern must stop here, not mis-hook.
  auto builder = ResolveSignature(server, "game-rules.manifest-builder");
  if (!builder.has_value()) {
    return std::unexpected(builder.error());
  }
  const auto builder_addr = reinterpret_cast<std::uintptr_t>(*builder);

  // Decode the precache pair from the builder's instruction stream.
  auto global = ResolveSignature(server, "game-rules.precache-global");
  if (!global.has_value()) {
    return std::unexpected(global.error());
  }
  auto fn = ResolveSignature(server, "game-rules.precache-call");
  if (!fn.has_value()) {
    return std::unexpected(fn.error());
  }
  auto add_resource = ResolveSignature(server, "game-rules.add-resource");
  if (!add_resource.has_value()) {
    return std::unexpected(add_resource.error());
  }

  // Stage 3: install. The thunk runs the original build first so the
  // manifest exists, then rides the same thread to add our heroes.
  auto created = safetyhook::create_inline(*builder, reinterpret_cast<void*>(&ManifestBuildThunk));
  if (!created) {
    return std::unexpected("inline hook on the manifest builder failed");
  }
  g_hook = std::move(created);

  // Keep the resolved calls and register the heroes.
  g_precache_global = *global;
  g_add_resource = reinterpret_cast<GameRulesHooks::AddResourceFn>(*add_resource);
  void* fn_addr = *fn;
  g_precache_fn = reinterpret_cast<HeroPrecacheFn>(fn_addr);
  GameRulesHooks owner(std::make_unique<Impl>());
  owner.AddPrecache(std::move(precache_heroes), std::move(resources));

  // Log where the hook landed.
  std::cout << "[modlock] game rules: manifest hook installed at +0x" << std::hex
            << builder_addr - server.base() << std::dec << "; " << g_hero_names.size()
            << " hero(es) registered\n";
  return owner;
#else
  (void)server;
  (void)precache_heroes;
  return std::unexpected("the game-rules hooks require the Windows host build");
#endif
}

}  // namespace modlock::gameinterop
