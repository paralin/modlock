#include "modlock/gameinterop/free_for_all_relations.h"

#include <cstdint>

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

#if defined(_WIN32)
// Classifier results for a hero target: kFriendlyHero when it shares the
// observer's team, kEnemyHero otherwise. Callers mask the result with the
// target types an ability accepts.
constexpr uint32_t kFriendlyHero = 0x1;
constexpr uint32_t kEnemyHero = 0x100;

// Target types an ability names for friendly units (the low six bits) and for
// enemy units, as the query filter reads them.
constexpr uint32_t kFriendlyTypes = 0x3f;
constexpr uint32_t kEnemyTypes = 0x13f00;

// QueryFilter is the head of the trace filter an ability's area query builds:
// the collision layers it visits and the ones it skips.
struct QueryFilter {
  void* vtable;
  uint64_t include;
  uint64_t exclude;
};

// Each team's own collision layer; the team table holds the same bits.
constexpr uint64_t TeamLayer(int team) {
  return team == 2 ? 0x80000000ull : team == 3 ? 0x100000000ull : 0;
}

bool g_enabled = false;
safetyhook::InlineHook* g_classify = nullptr;
safetyhook::InlineHook* g_filter = nullptr;
safetyhook::InlineHook* g_collision = nullptr;

uint32_t ClassifyThunk(void* target, uint32_t team) {
  const auto kind = g_classify->call<uint32_t>(target, team);
  return g_enabled && kind == kFriendlyHero ? kFriendlyHero | kEnemyHero : kind;
}

void FilterThunk(int team, void* caster, uint32_t types, void* arg, QueryFilter* filter) {
  g_filter->call<void>(team, caster, types, arg, filter);
  // An enemy-only query skips the caster's own layer; visit it too. A query
  // that already names friendly types visits it already.
  const auto layer = TeamLayer(team);
  if (g_enabled && layer != 0 && (types & kFriendlyTypes) == 0 && (types & kEnemyTypes) != 0) {
    filter->include |= layer;
    filter->exclude &= ~layer;
  }
}

void CollisionThunk(void* entity, bool keep_own_team) {
  g_collision->call<void>(entity, keep_own_team || g_enabled);
}
#endif

}  // namespace

struct FreeForAllRelations::Impl {
#if defined(_WIN32)
  safetyhook::InlineHook classify;
  safetyhook::InlineHook filter;
  safetyhook::InlineHook collision;

  ~Impl() {
    if (g_classify != &classify) {
      return;
    }
    g_enabled = false;
    collision.reset();
    filter.reset();
    classify.reset();
    g_classify = nullptr;
    g_filter = nullptr;
    g_collision = nullptr;
  }
#endif
};

FreeForAllRelations::FreeForAllRelations(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FreeForAllRelations::FreeForAllRelations(FreeForAllRelations&&) noexcept = default;
FreeForAllRelations& FreeForAllRelations::operator=(FreeForAllRelations&&) noexcept = default;
FreeForAllRelations::~FreeForAllRelations() = default;

void FreeForAllRelations::SetEnabled(bool enabled) {
#if defined(_WIN32)
  g_enabled = enabled;
#else
  (void)enabled;
#endif
}

std::expected<FreeForAllRelations, std::string> FreeForAllRelations::Install(
    const ModuleImage& server) {
#if defined(_WIN32)
  // Install once.
  if (g_classify) {
    return std::unexpected("free-for-all relations are already installed");
  }

  // Resolve every target and create each hook disabled, so a failure leaves
  // the game stock.
  struct Target {
    const char* id;
    void* thunk;
    safetyhook::InlineHook Impl::* hook;
  };

  const Target targets[] = {
      {"team.classify-target", reinterpret_cast<void*>(&ClassifyThunk), &Impl::classify},
      {"team.ability-query-filter", reinterpret_cast<void*>(&FilterThunk), &Impl::filter},
      {"team.collision", reinterpret_cast<void*>(&CollisionThunk), &Impl::collision},
  };
  auto impl = std::make_unique<Impl>();
  for (const auto& target : targets) {
    auto address = ResolveSignature(server, target.id);
    if (!address) {
      return std::unexpected(address.error());
    }
    auto hook =
        safetyhook::create_inline(*address, target.thunk, safetyhook::InlineHook::StartDisabled);
    if (!hook) {
      return std::unexpected(std::string(target.id) + " hook creation failed");
    }
    (*impl).*target.hook = std::move(hook);
  }

  // Publish the thunk state and enable.
  g_classify = &impl->classify;
  g_filter = &impl->filter;
  g_collision = &impl->collision;
  for (const auto& target : targets) {
    if (auto enabled = ((*impl).*target.hook).enable(); !enabled) {
      return std::unexpected(std::string(target.id) + " hook activation failed");
    }
  }
  return FreeForAllRelations(std::move(impl));

#else
  // Other platforms have no native server.
  (void)server;
  return std::unexpected("free-for-all relations require the Windows host");
#endif
}

}  // namespace modlock::gameinterop
