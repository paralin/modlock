#include "modlock/gameinterop/pawn_observer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <span>
#include <utility>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "pawn_observer_internal.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace modlock::gameinterop {

namespace {

// Trace writes one interop trace line to stderr and flushes it, so the line
// survives a crash that follows it.
template <typename... Args>
void Trace(std::format_string<Args...> format, Args&&... args) {
  const auto line = std::format(format, std::forward<Args>(args)...);
  std::fprintf(stderr, "[modlock] %s\n", line.c_str());
  std::fflush(stderr);
}

// ReadUint32 reads the unsigned integer at offset from base.
uint32_t ReadUint32(const unsigned char* base, size_t offset) {
  uint32_t value = 0;
  std::memcpy(&value, base + offset, sizeof(value));
  return value;
}

// The EModifierState values of the native dash and climb action states.
constexpr uint16_t kGroundDashingState = 0x25;
constexpr uint16_t kAirDashingState = 0x29;
constexpr uint16_t kClimbingState = 0xbc;

// ReadMovementState reads the pawn's movement fields, or is empty when
// they did not resolve.
std::optional<PawnObserver::Sample::MovementState> ReadMovementState(const EntityLayout& layout,
                                                                     void* pawn) {
  // Without movement fields there is no movement state.
  if (!layout.movement.has_value()) {
    return std::nullopt;
  }

  // Read velocity, ground and action states where their fields exist.
  const MovementLayout& movement = *layout.movement;
  const auto* pawn_bytes = static_cast<const unsigned char*>(pawn);
  PawnObserver::Sample::MovementState state;
  if (movement.abs_velocity.has_value()) {
    std::array<float, 3> velocity{};
    std::memcpy(velocity.data(), pawn_bytes + *movement.abs_velocity, sizeof(velocity));
    if (std::isfinite(velocity[0]) && std::isfinite(velocity[1]) && std::isfinite(velocity[2])) {
      state.abs_velocity = velocity;
    }
  }
  if (movement.ground_entity.has_value()) {
    state.ground_entity_handle = ReadUint32(pawn_bytes, *movement.ground_entity);
    state.grounded_by_handle = *state.ground_entity_handle != kInvalidEHandleIndex;
  }
  if (movement.modifier_property && movement.modifier_state_mask) {
    const unsigned char* property = nullptr;
    std::memcpy(&property, pawn_bytes + *movement.modifier_property, sizeof(property));
    if (property) {
      const auto enabled = [&](uint16_t flag) {
        const auto word = ReadUint32(property, *movement.modifier_state_mask + (flag / 32) * 4);
        return (word & (uint32_t{1} << (flag % 32))) != 0;
      };
      state.dashing = enabled(kGroundDashingState) || enabled(kAirDashingState);
      state.climbing = enabled(kClimbingState);
    }
  }
  return state;
}

// ReadEyePosition offsets origin by the pawn's view offset, or is empty when
// the field did not resolve or the result is not finite.
std::optional<std::array<float, 3>> ReadEyePosition(const EntityLayout& layout,
                                                    const unsigned char* pawn,
                                                    const float* origin) {
  // Without a view offset there is no eye position.
  if (!layout.view_offset) {
    return std::nullopt;
  }

  // Offset each network float from the origin.
  std::array<float, 3> eye{};
  for (size_t axis = 0; axis < eye.size(); ++axis) {
    const size_t at = *layout.view_offset + kViewOffsetFirstFloat + axis * kViewOffsetFloatStride;
    std::memcpy(&eye[axis], pawn + at, sizeof(float));
    eye[axis] += origin[axis];
  }
  if (!AllFinite(eye)) {
    return std::nullopt;
  }
  return eye;
}

// ReadCurrencies reads the pawn's currencies when their field resolved.
std::optional<std::array<int32_t, 3>> ReadCurrencies(const EntityLayout& layout,
                                                     const unsigned char* pawn) {
  if (!layout.currencies) {
    return std::nullopt;
  }
  std::array<int32_t, 3> currencies{};
  std::memcpy(currencies.data(), pawn + *layout.currencies, sizeof(currencies));
  return currencies;
}

// ReadScore reads the scoreboard totals from the controller's player data. It
// borrows the engine-owned controller only for the read.
std::optional<ScoreTotals> ReadScore(const EntityLayout& layout, const void* controller) {
  // Without score fields there are no totals.
  if (!layout.score) {
    return std::nullopt;
  }

  // Read each counter from the player data.
  const ScoreLayout& offsets = *layout.score;
  const auto* data = static_cast<const unsigned char*>(controller) + offsets.player_data_global;
  ScoreTotals totals;
  for (const auto& counter : kScoreCounters) {
    std::memcpy(&(totals.*counter.total), data + offsets.*counter.offset, sizeof(int32_t));
  }
  return totals;
}

// ReadStamina reads the pawn's native stamina. It is empty when the field did
// not resolve or the values are not sane, never a fabricated default.
std::optional<PawnObserver::Sample::Stamina> ReadStamina(const EntityLayout& layout,
                                                         const unsigned char* pawn) {
  // Without stamina fields there is no stamina.
  if (!layout.stamina) {
    return std::nullopt;
  }

  // Read the current and maximum stamina.
  float current = 0;
  float max = 0;
  std::memcpy(&current, pawn + layout.stamina->current, sizeof(current));
  std::memcpy(&max, pawn + layout.stamina->max, sizeof(max));
  if (!std::isfinite(current) || !std::isfinite(max) || max <= 0) {
    return std::nullopt;
  }
  return PawnObserver::Sample::Stamina{.current = current, .max = max};
}

// ApplyMovementAbilities copies the mantle, slide and jump abilities' state
// into movement. The jump state is copied only from a unique jump ability.
void ApplyMovementAbilities(PawnObserver::Sample::MovementState& movement,
                            std::span<const PawnObserver::Ability> abilities) {
  // Mantle and slide read from their slots; find the jump ability.
  const PawnObserver::Ability* jump = nullptr;
  bool ambiguous = false;
  for (const auto& ability : abilities) {
    if (ability.slot == kMantleSlot) {
      movement.mantling = ability.channeling;
      movement.mantle_ability_handle = ability.handle;
      movement.mantle_start_time = ability.mantle_start_time;
    }
    if (ability.slot == kSlideSlot) {
      movement.sliding = ability.sliding;
    }
    if (ability.slot == kJumpSlot) {
      ambiguous = jump != nullptr;
      jump = &ability;
    }
  }
  if (jump == nullptr || ambiguous) {
    return;
  }

  // Copy the jump's wall and zipline state.
  if (jump->consecutive_wall_jumps) {
    movement.jump_ability_handle = jump->handle;
    movement.consecutive_wall_jumps = jump->consecutive_wall_jumps;
  }
  movement.current_wall_normal = jump->current_wall_normal;
  movement.wall_contact_position = jump->wall_contact_position;
  movement.wall_jump_normal_used = jump->wall_jump_normal_used;
  movement.wall_jump_facing = jump->wall_jump_facing;
  movement.last_time_on_zipline = jump->last_time_on_zipline;
}

}  // namespace

unsigned char* EntityListOf(void* entity_system) {
  return static_cast<unsigned char*>(entity_system) + kEntityListOffset;
}

unsigned char* IdentityOfIndex(unsigned char* entity_list, uint32_t index) {
  // Find the identity's slot in its chunk.
  if (index >= kMaxEntitiesInList * kMaxEntityLists) {
    return nullptr;
  }
  auto* chunks = reinterpret_cast<unsigned char**>(entity_list);
  unsigned char* chunk = chunks[index / kMaxEntitiesInList];
  if (chunk == nullptr) {
    return nullptr;
  }
  unsigned char* identity = chunk + (index % kMaxEntitiesInList) * kIdentitySize;

  // The identity must hold this index and a live instance.
  uint32_t ehandle = 0;
  std::memcpy(&ehandle, identity + kIdentityEHandleOffset, sizeof(ehandle));
  if (EntityIndexOf(ehandle) != index) {
    return nullptr;
  }
  void* instance = nullptr;
  std::memcpy(&instance, identity + kIdentityInstanceOffset, sizeof(instance));
  if (instance == nullptr) {
    return nullptr;
  }

  // CEntityInstance::m_pEntity must point back at the identity that produced
  // the instance (sourcesdk public/entity2/entityinstance.h).
  void* back = nullptr;
  std::memcpy(&back, static_cast<unsigned char*>(instance) + kIdentityOffset, sizeof(back));
  if (back != identity) {
    return nullptr;
  }
  return identity;
}

unsigned char* IdentityOfHandle(unsigned char* entity_list, uint32_t handle) {
  // Find the identity at the handle's index.
  if (handle == kInvalidEHandleIndex) {
    return nullptr;
  }
  const uint32_t index = EntityIndexOf(handle);
  auto* identity = IdentityOfIndex(entity_list, index);
  if (identity == nullptr) {
    return nullptr;
  }

  // The identity must be live and carry the handle's serial.
  uint32_t ehandle = 0;
  std::memcpy(&ehandle, identity + kIdentityEHandleOffset, sizeof(ehandle));
  uint32_t flags = 0;
  std::memcpy(&flags, identity + kIdentityFlagsOffset, sizeof(flags));
  if (LiveHandleOf(ehandle, flags) != handle) {
    return nullptr;
  }
  return identity;
}

void* InstanceOf(unsigned char* identity) {
  void* instance = nullptr;
  std::memcpy(&instance, identity + kIdentityInstanceOffset, sizeof(instance));
  return instance;
}

const float* AbsOriginOf(const EntityLayout& layout, void* pawn) {
  // Follow the body component to its scene node's origin.
  void* body = nullptr;
  std::memcpy(&body, static_cast<const unsigned char*>(pawn) + layout.body_component, sizeof(body));
  if (body == nullptr) {
    return nullptr;
  }
  void* scene_node = nullptr;
  std::memcpy(&scene_node, static_cast<const unsigned char*>(body) + layout.scene_node,
              sizeof(scene_node));
  if (scene_node == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<const float*>(static_cast<const unsigned char*>(scene_node) +
                                        layout.abs_origin);
}

bool AllFinite(std::span<const float> values) {
  return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}

PawnObserver::Seams PawnObserver::LiveSeams(bool movement_enabled) {
  // Read entities from the live entity system.
  Seams seams;
  seams.entity_system = [] {
    return ResolveLiveEntitySystem();
  };

  // Occupancy and xuid come from the engine-reported client lifecycle only.
  seams.slot_state = [](int32_t slot) {
    return ConnectionTracker::StateForSlot(slot);
  };

  // Resolve the layouts from the server's schema.
  seams.layout = [movement_enabled] {
    return ResolveEntityLayout(movement_enabled);
  };
  seams.ability_layout = [] {
    return ResolveAbilityLayout();
  };

  // Read the calculated maximum health through the pawn's override.
  seams.effective_max_health = [](void* pawn) -> std::optional<int32_t> {
    // Find the pawn's maximum health override, resolved once since every
    // observation reads it.
    if (!pawn) {
      return std::nullopt;
    }
    static const auto slot = ResolveVirtualSlot("entity.max-health");
    if (!slot) {
      return std::nullopt;
    }
    auto** table = *static_cast<void***>(pawn);
    if (!table || !table[*slot]) {
      return std::nullopt;
    }

    // The pawn override evaluates progression and modifiers; m_iMaxHealth
    // can remain zero even while this calculated maximum is positive.
    using GetMaxHealth = int32_t (*)(void*);
    const int32_t value = reinterpret_cast<GetMaxHealth>(table[*slot])(pawn);
    return value > 0 ? std::optional<int32_t>{value} : std::nullopt;
  };

  // Resolve once per observer; the loaded server module outlives its pawn seams.
  seams.max_charges = [slots = std::optional<std::expected<AbilityChargeSlots, std::string>>{}](
                          void* ability) mutable -> std::expected<int32_t, std::string> {
#if defined(_WIN32)
    // Resolve the charge query slots once.
    const auto server = reinterpret_cast<const unsigned char*>(GetModuleHandleW(L"server.dll"));
    if (!server || !ability) {
      return std::unexpected("native charge capability unavailable");
    }
    if (!slots) {
      const auto image = MappedModuleImage::ForModule(L"server.dll");
      if (!image) {
        return std::unexpected(image.error());
      }
      slots = ResolveAbilityChargeSlots(*image);
    }
    if (!*slots) {
      return std::unexpected(slots->error());
    }
    const auto queries = **slots;

    // Both queries must run mapped server code.
    auto** table = *static_cast<void***>(ability);
    MEMORY_BASIC_INFORMATION memory{};
    if (!table || !VirtualQuery(table, &memory, sizeof(memory)) ||
        memory.AllocationBase != server || memory.State != MEM_COMMIT ||
        reinterpret_cast<uintptr_t>(table) +
                (std::max(queries.has_charges, queries.max_charges) + 1) * sizeof(void*) >
            reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize) {
      return std::unexpected("ability charge vtable is outside the mapped server");
    }
    for (const auto slot : {queries.has_charges, queries.max_charges}) {
      if (!VirtualQuery(table[slot], &memory, sizeof(memory)) || memory.AllocationBase != server ||
          memory.State != MEM_COMMIT ||
          !(memory.Protect &
            (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        return std::unexpected("ability charge target is not mapped server code");
      }
    }

    // Read the maximum, or zero for an ability without charges.
    using HasCharges = bool (*)(void*);
    using MaxCharges = int32_t (*)(void*);
    if (!reinterpret_cast<HasCharges>(table[queries.has_charges])(ability)) {
      return 0;
    }
    const auto maximum = reinterpret_cast<MaxCharges>(table[queries.max_charges])(ability);
    if (maximum <= 0) {
      return std::unexpected("native ability charge maximum is invalid");
    }
    return maximum;
#else
    return std::unexpected("native ability charge capability requires Windows");
#endif
  };

  // Resolve the native lookups ahead of the first observation.
  seams.prepare = [] {
    static_cast<void>(ResolveVirtualSlot("entity.max-health"));
    PrepareUpgradeBonuses();
  };
  return seams;
}

PawnObserver::PawnObserver() : PawnObserver(LiveSeams(), false) {}

PawnObserver::PawnObserver(Seams seams, bool movement_enabled)
    : seams_(std::move(seams)), movement_enabled_(movement_enabled) {}

void PawnObserver::NoteDegradation(const char* reason) {
  if (!degradation_logged_) {
    degradation_logged_ = true;
    std::fprintf(stderr, "[modlock] pawn observation degraded: %s\n", reason);
    std::fflush(stderr);
  }
}

std::expected<void, std::string> PawnObserver::ResolveLayout() {
  // Resolve the layout once and keep it.
  if (offsets_ok_) {
    return {};
  }
  if (!seams_.layout) {
    return std::unexpected("player layout unavailable");
  }
  auto layout = seams_.layout();
  if (!layout) {
    return std::unexpected(layout.error());
  }
  layout_ = *layout;
  offsets_ok_ = true;
  return {};
}

void PawnObserver::Prepare() {
  static_cast<void>(ResolveLayout());
  if (seams_.prepare) {
    seams_.prepare();
  }
}

uint32_t PawnObserver::ObservedPawnHandle(const void* controller) const {
  // Death or a watch can switch the active pawn to a spectator or a camera
  // while the hero remains available for observation and round restoration.
  const auto* bytes = static_cast<const unsigned char*>(controller);
  const size_t field = bytes[layout_.team] == 1
                           ? layout_.pawn_handle
                           : layout_.hero_pawn_handle.value_or(layout_.pawn_handle);
  uint32_t handle = 0;
  std::memcpy(&handle, bytes + field, sizeof(handle));
  return handle;
}

std::optional<PawnObserver::Sample> PawnObserver::Observe(int32_t slot) {
  // The stage trace covers the first ten occupied frames: frame one names the
  // walk stages, and the window distinguishes a pre-spawn zero origin from a
  // dead offset.
  static const bool stage_trace = InteropTraceEnabled();
  static int traced_frames = 0;
  const bool trace_now = stage_trace && traced_frames < 10;

  // Slot occupancy comes from the engine-reported client lifecycle, never
  // from entity bytes. An occupied-to-empty transition is the confirmed
  // disconnect that rearms placement; an empty slot while already empty
  // changes nothing.
  auto slot_state = seams_.slot_state ? seams_.slot_state(slot) : ConnectionTracker::SlotState{};
  if (!slot_state.occupied) {
    if (stage_trace && trace_slot_occupied_) {
      Trace("interop trace: pawn slot {} state occupied->empty generation {}", slot,
            slot_state.generation);
      trace_slot_occupied_ = false;
    }
    return std::nullopt;
  }
  if (stage_trace && !trace_slot_occupied_) {
    Trace("interop trace: pawn slot {} state empty->occupied generation {}", slot,
          slot_state.generation);
    trace_slot_occupied_ = true;
  }

  // Resolve the layout once.
  if (auto layout = ResolveLayout(); !layout) {
    NoteDegradation(layout.error().c_str());
    return std::nullopt;
  }

  // Resolve the entity list.
  if (!seams_.entity_system) {
    NoteDegradation("no entity-system resolver");
    return std::nullopt;
  }
  auto entity_system = seams_.entity_system();
  if (!entity_system) {
    NoteDegradation(entity_system.error().c_str());
    return std::nullopt;
  }
  unsigned char* entity_list = EntityListOf(entity_system.value());

  // The controller for a Source player slot lives at entity index slot+1.
  auto* controller_identity = IdentityOfIndex(entity_list, slot + 1);
  if (controller_identity == nullptr) {
    if (trace_now) {
      ++traced_frames;
      Trace("observe trace: controller identity at index {} did not resolve", slot + 1);
    }
    return std::nullopt;
  }
  auto* controller = InstanceOf(controller_identity);
  if (controller == nullptr) {
    return std::nullopt;
  }

  // Read the controller's pawn handle.
  const uint32_t pawn_handle = ObservedPawnHandle(controller);
  if (trace_now) {
    ++traced_frames;
    Trace("observe trace: frame {} pawn handle = {:#010x} (index {} serial {})", traced_frames,
          pawn_handle, EntityIndexOf(pawn_handle), pawn_handle >> kSerialShift);
  }

  // Resolve the pawn and its origin, tracing each failure.
  auto* pawn_identity = IdentityOfHandle(entity_list, pawn_handle);
  if (pawn_identity == nullptr) {
    if (trace_now) {
      Trace("observe trace: pawn identity failed the serial proof");
    }
    return std::nullopt;
  }
  void* pawn = InstanceOf(pawn_identity);
  if (pawn == nullptr) {
    if (trace_now) {
      Trace("observe trace: pawn identity carries no instance");
    }
    return std::nullopt;
  }
  const float* origin = AbsOriginOf(layout_, pawn);
  if (origin == nullptr) {
    if (trace_now) {
      Trace("observe trace: pawn origin chain is incomplete");
    }
    return std::nullopt;
  }
  if (trace_now) {
    Trace("observe trace: full walk ok, origin {:.1f} {:.1f} {:.1f} xuid {}", origin[0], origin[1],
          origin[2], slot_state.xuid);
  }
  if (!AllFinite(std::span<const float>(origin, 3))) {
    return std::nullopt;
  }

  // Movement is read directly from the pawn. In particular, velocity is not
  // reconstructed from origin deltas when this optional layout is absent.
  auto movement = movement_enabled_ ? ReadMovementState(layout_, pawn) : std::nullopt;

  // ConnectionTracker's generation changes for every accepted client,
  // including a same-XUID reconnect that occurs between observer frames.
  const uint64_t xuid = slot_state.xuid;
  uint32_t hero_id = 0;
  uint8_t team = 0;
  int32_t health = 0, max_health = 0, level = 0;

  // Read the hero, team, health and level.
  const auto* bytes = static_cast<const unsigned char*>(pawn);
  std::memcpy(&hero_id, bytes + layout_.hero_id, sizeof(hero_id));
  std::memcpy(&team, bytes + layout_.team, sizeof(team));
  std::memcpy(&health, bytes + layout_.health, sizeof(health));
  std::memcpy(&max_health, bytes + layout_.max_health, sizeof(max_health));
  std::memcpy(&level, bytes + layout_.level, sizeof(level));

  // Read the eye and camera angles, and the eye position when the view offset resolves.
  std::array<float, 3> eye_angles, camera_angles;
  std::memcpy(eye_angles.data(), bytes + layout_.eye_angles, sizeof(eye_angles));
  std::memcpy(camera_angles.data(), bytes + layout_.camera_angles, sizeof(camera_angles));
  const auto eye_position = ReadEyePosition(layout_, bytes, origin);

  // Reject a pose whose angles are not finite.
  if (!AllFinite(camera_angles) || !AllFinite(eye_angles)) {
    return std::nullopt;
  }

  // Read the movement abilities when the movement state resolved.
  if (movement) {
    const Located here{pawn, entity_list, pawn_handle, slot_state.generation};
    if (const auto abilities = AbilitiesOf(here)) {
      ApplyMovementAbilities(*movement, *abilities);
    }
  }
  return Sample{.slot = slot,
                .x = origin[0],
                .y = origin[1],
                .z = origin[2],
                .steam_id = xuid,
                .session_generation = slot_state.generation,
                .pawn_handle = pawn_handle,
                .pawn = pawn,
                .movement = std::move(movement),
                .hero_id = hero_id,
                .team = team,
                .health = health,
                .max_health = max_health,
                .level = level,
                .currencies = ReadCurrencies(layout_, bytes),
                .eye_angles = eye_angles,
                .camera_angles = camera_angles,
                .eye_position = eye_position,
                .is_bot = slot_state.is_bot,
                .effective_max_health =
                    seams_.effective_max_health ? seams_.effective_max_health(pawn) : std::nullopt,
                .upgrade_bonuses = ReadPawnUpgradeBonuses(pawn),
                .score = ReadScore(layout_, controller),
                .stamina = ReadStamina(layout_, bytes)};
}

void PawnObserver::Invalidate() {
  // Forget every resolved layout.
  offsets_ok_ = false;
  layout_ = {};
  ability_layout_.reset();
  trace_slot_occupied_ = false;
}

std::expected<PawnObserver::Located, std::string> PawnObserver::Locate(int32_t slot) const {
  // The slot needs a connected client and a resolved layout.
  const auto connection =
      seams_.slot_state ? seams_.slot_state(slot) : ConnectionTracker::SlotState{};
  if (!connection.occupied) {
    return std::unexpected("player slot is empty");
  }
  if (!offsets_ok_ || !seams_.entity_system) {
    return std::unexpected("pawn layout is unresolved");
  }

  // Find the controller at index slot + 1.
  auto system = seams_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  auto* list = EntityListOf(*system);
  auto* controller_identity = IdentityOfIndex(list, slot + 1);
  auto* controller = controller_identity ? InstanceOf(controller_identity) : nullptr;
  if (!controller) {
    return std::unexpected("player controller unavailable");
  }

  // Resolve the pawn it selects through the serial proof.
  const uint32_t handle = ObservedPawnHandle(controller);
  auto* pawn_identity = IdentityOfHandle(list, handle);
  void* pawn = pawn_identity ? InstanceOf(pawn_identity) : nullptr;
  if (!pawn) {
    return std::unexpected("player pawn is not live");
  }
  return Located{pawn, list, handle, connection.generation};
}

std::expected<PawnObserver::Located, std::string> PawnObserver::Relocate(
    int32_t slot, const Located& before) const {
  auto here = Locate(slot);
  if (!here || here->pawn_handle != before.pawn_handle || here->generation != before.generation) {
    return std::unexpected("player pawn changed during the call");
  }
  return here;
}

void* PawnObserver::PawnForSlot(int32_t slot) const {
  const auto here = Locate(slot);
  return here ? here->pawn : nullptr;
}

}  // namespace modlock::gameinterop
