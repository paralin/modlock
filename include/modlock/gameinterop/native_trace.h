#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

using TraceVector = std::array<float, 3>;

struct TraceLine {
  TraceVector offset{};
  float radius = 0;
};

struct TraceSphere {
  TraceVector center{};
  float radius = 0;
};

struct TraceHull {
  TraceVector mins{};
  TraceVector maxs{};
};

struct TraceCapsule {
  TraceVector center_a{};
  TraceVector center_b{};
  float radius = 0;
};

// TraceMesh borrows its vertices only for the synchronous query.
struct TraceMesh {
  TraceVector mins{};
  TraceVector maxs{};
  std::span<const TraceVector> vertices;
};

using TraceShape = std::variant<TraceLine, TraceSphere, TraceHull, TraceCapsule, TraceMesh>;

// TraceLayer matches the installed game's 64 interaction layers.
enum class TraceLayer : std::uint8_t {
  kSolid,
  kHitbox,
  kTrigger,
  kSky,
  kPlayerClip,
  kNpcClip,
  kBlockLos,
  kBlockLight,
  kLadder,
  kPickup,
  kBlockSound,
  kNoDraw,
  kWindow,
  kPassBullets,
  kWorldGeometry,
  kWater,
  kSlime,
  kTouchAll,
  kPlayer,
  kNpc,
  kDebris,
  kPhysicsProp,
  kNavIgnore,
  kNavLocalIgnore,
  kPostProcessingVolume,
  kUnusedLayer3,
  kCarriedObject,
  kPushaway,
  kServerEntityOnClient,
  kCarriedWeapon,
  kStaticLevel,
  kTeamAmber,
  kTeamSapphire,
  kTeamNeutral,
  kAbility,
  kBullet,
  kProjectile,
  kUnitHero,
  kUnitTrooper,
  kUnitNeutral,
  kUnitBuilding,
  kUnitProp,
  kUnitMinion,
  kUnitBoss,
  kUnitGoldOrb,
  kUnitWorldProp,
  kUnitTrophy,
  kUnitZipline,
  kMantleHidden,
  kObscured,
  kTimeWarp,
  kFoliage,
  kTransparent,
  kBlockCamera,
  kMantleable,
  kWalkable,
  kTempMovementBlocker,
  kBlockMantle,
  kSkyclip,
  kValidPingTarget,
  kCameraCanPassThrough,
  kAbilityTrigger,
  kPortalTrigger,
  kPortalEnvironment,
};

constexpr std::uint64_t TraceMask(TraceLayer layer) {
  return std::uint64_t{1} << static_cast<std::uint8_t>(layer);
}

// TraceOptions selects collision layers and the same filters as VPhys2.
// Ignored entity handles include their serial; the adapter extracts indices.
struct MODLOCK_API TraceOptions {
  std::uint64_t interacts_with = TraceMask(TraceLayer::kSolid) | TraceMask(TraceLayer::kHitbox);
  std::uint64_t interacts_exclude = 0;
  std::uint64_t interacts_as = 0;
  std::array<std::uint32_t, 2> ignored_entities{UINT32_MAX, UINT32_MAX};
  std::array<std::uint32_t, 2> ignored_owners{UINT32_MAX, UINT32_MAX};
  std::array<std::uint16_t, 2> ignored_hierarchies{};
  std::uint16_t included_detail_layers = UINT16_MAX;
  std::uint8_t target_detail_layer = 0;
  // Object bits: static, keyframed, dynamic, locatable.
  std::uint8_t object_set = 0x0f;
  // Group 3 is the one the game's own traces pass most. Since build 6801 a
  // group 0 query hits every trigger volume whatever its layers.
  std::uint8_t collision_group = 3;
  bool hit_solid = true;
  bool require_contacts = false;
  bool hit_trigger = false;
  bool ignore_disabled_pairs = true;
  bool ignore_shared_hitboxes = false;
  bool force_hit_everything = false;
  bool iterate_entities = true;
};

// TraceResult owns the hit facts; no borrowed engine pointer escapes the query.
struct MODLOCK_API TraceResult {
  TraceVector position{};
  TraceVector normal{};
  TraceVector start{};
  TraceVector end{};
  TraceVector exact_position{};
  float fraction = 1;
  float hit_offset = 0;
  std::uint64_t contents = 0;
  std::optional<std::uint32_t> entity_handle;
  std::int32_t triangle = -1;
  std::int16_t hitbox_bone = -1;
  bool start_in_solid = false;
  bool exact_hit_point = false;

  bool DidHit() const { return fraction < 1 || start_in_solid; }
};

// NativeTrace calls the server's VPhys2 shape query on the loaded world.
// Install resolves it from the server module; Query runs on the engine frame
// thread and works whenever a world is loaded.
class MODLOCK_API NativeTrace {
 public:
  static std::expected<NativeTrace, std::string> Install(const ModuleImage& server);

  NativeTrace(NativeTrace&&) noexcept;
  NativeTrace& operator=(NativeTrace&&) noexcept;
  ~NativeTrace();

  std::expected<TraceResult, std::string> Query(const TraceVector& start, const TraceVector& end,
                                                const TraceShape& shape = TraceLine{},
                                                const TraceOptions& options = {}) const;
  // IsReady reports whether Query can run: a world is loaded.
  [[nodiscard]] bool IsReady() const;
  std::expected<TraceResult, std::string> RayAngles(const TraceVector& start,
                                                    const TraceVector& angles,
                                                    float distance = 8192,
                                                    const TraceOptions& options = {}) const;

 private:
  struct Impl;
  explicit NativeTrace(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
