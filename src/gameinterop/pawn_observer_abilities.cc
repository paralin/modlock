#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <span>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "pawn_observer_internal.h"
#include "utl_vector.h"

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {

namespace {

#if defined(_WIN32)
using NativeCreateAbility = void* (*)(void*, void*, uint16_t, uint64_t, bool, void*);

// SlotRequest is scoped to one mod creation on the engine thread. Native
// auxiliary creations and allocations on other threads keep their own rules.
struct SlotRequest {
  void* component;
  void* definition;
  uint16_t slot;
};

thread_local const SlotRequest* g_slot_request = nullptr;

class ScopedSlotRequest {
 public:
  explicit ScopedSlotRequest(const SlotRequest& request) : previous_(g_slot_request) {
    g_slot_request = &request;
  }

  ~ScopedSlotRequest() { g_slot_request = previous_; }

 private:
  const SlotRequest* previous_;
};

// SlotSelection lives while any resolved creator holds it. Callers release
// creators at world shutdown, before the borrowed server module unloads.
struct SlotSelection;
SlotSelection* g_slot_selection = nullptr;
std::weak_ptr<SlotSelection> g_shared_slot_selection;

struct SlotSelection {
  void* target = nullptr;
  safetyhook::InlineHook hook;

  ~SlotSelection() {
    hook.reset();
    if (g_slot_selection == this) {
      g_slot_selection = nullptr;
    }
  }

  void* Create(NativeCreateAbility create, void* component, void* definition, uint16_t slot,
               uint64_t upgrade, bool flag, void* extra) const {
    const SlotRequest request{component, definition, slot};
    const ScopedSlotRequest scoped(request);
    return create(component, definition, slot, upgrade, flag, extra);
  }
};

int32_t SelectSlotThunk(void* component, void* definition, uint16_t slot) {
  const auto* request = g_slot_request;
  if (request && slot < 4 && request->component == component && request->definition == definition &&
      request->slot == slot) {
    return slot;
  }
  return g_slot_selection->hook.call<int32_t>(component, definition, slot);
}

std::expected<std::shared_ptr<SlotSelection>, std::string> ResolveSlotSelection(
    const ModuleImage& server) {
  // Resolve the allocator's own slot-selection call before touching its code.
  auto target = ResolveSignature(server, "ability.select-slot");
  if (!target) {
    return std::unexpected(target.error());
  }
  if (auto shared = g_shared_slot_selection.lock()) {
    if (shared->target != *target) {
      return std::unexpected("ability slot selection belongs to another server module");
    }
    return shared;
  }

  // Publish the owner before enabling the detour; failure removes it again.
  auto selection = std::make_shared<SlotSelection>();
  selection->target = *target;
  selection->hook = safetyhook::create_inline(*target, reinterpret_cast<void*>(&SelectSlotThunk),
                                              safetyhook::InlineHook::StartDisabled);
  if (!selection->hook) {
    return std::unexpected("cannot create ability slot selection hook");
  }
  g_slot_selection = selection.get();
  if (!selection->hook.enable()) {
    return std::unexpected("cannot enable ability slot selection hook");
  }
  g_shared_slot_selection = selection;
  return selection;
}
#endif

// ReadFinite reads the float at an optional offset, or is empty when the
// offset did not resolve or the value is not finite.
std::optional<float> ReadFinite(const unsigned char* entity, std::optional<size_t> offset) {
  // An unresolved offset has no value.
  if (!offset) {
    return std::nullopt;
  }

  // Read the value and keep it when finite.
  float value = 0;
  std::memcpy(&value, entity + *offset, sizeof(value));
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

// ReadFiniteVector reads the vector at an optional offset, or is empty when
// the offset did not resolve or a component is not finite.
std::optional<std::array<float, 3>> ReadFiniteVector(const unsigned char* entity,
                                                     std::optional<size_t> offset) {
  // An unresolved offset has no value.
  if (!offset) {
    return std::nullopt;
  }

  // Read the value and keep it when finite.
  std::array<float, 3> value{};
  std::memcpy(value.data(), entity + *offset, sizeof(value));
  if (!AllFinite(value)) {
    return std::nullopt;
  }
  return value;
}

// ReadMovementSlotState reads the channel state every ability carries and the
// mantle, slide, wall jump and zipline state its movement slot carries.
void ReadMovementSlotState(const AbilityLayout& layout, const unsigned char* entity,
                           PawnObserver::Ability& ability) {
  // Read the channel, mantle and slide state.
  if (layout.channeling) {
    ability.channeling = entity[*layout.channeling] == 1;
  }
  if (ability.slot == kMantleSlot) {
    ability.mantle_start_time = ReadFinite(entity, layout.mantle_start_time);
  }
  if (ability.slot == kSlideSlot && layout.sliding) {
    ability.sliding = entity[*layout.sliding] == 1;
  }
  if (ability.slot != kJumpSlot) {
    return;
  }

  // The jump slot carries the wall jump count and the wall and zipline state.
  if (layout.consecutive_wall_jumps) {
    int8_t count = 0;
    std::memcpy(&count, entity + *layout.consecutive_wall_jumps, sizeof(count));
    if (count >= 0) {
      ability.consecutive_wall_jumps = count;
    }
  }
  ability.current_wall_normal = ReadFiniteVector(entity, layout.current_wall_normal);
  ability.wall_contact_position = ReadFiniteVector(entity, layout.wall_contact_position);
  ability.wall_jump_normal_used = ReadFiniteVector(entity, layout.wall_jump_normal_used);
  if (layout.wall_jump_facing) {
    uint16_t facing = 0;
    std::memcpy(&facing, entity + *layout.wall_jump_facing, sizeof(facing));
    ability.wall_jump_facing = facing;
  }
  ability.last_time_on_zipline = ReadFinite(entity, layout.last_time_on_zipline);
}

// ReadOwnedAbility reads the ability entity with handle. It fails unless the
// ability's timers are finite, pawn_handle owns it and it has a subclass.
std::expected<PawnObserver::Ability, std::string> ReadOwnedAbility(const AbilityLayout& layout,
                                                                   const unsigned char* entity,
                                                                   uint32_t handle,
                                                                   uint32_t pawn_handle) {
  // Read its owner, identity, slot, upgrades and charges.
  uint32_t owner = 0;
  PawnObserver::Ability ability{.handle = handle};
  std::memcpy(&owner, entity + layout.owner, sizeof(owner));
  std::memcpy(&ability.subclass_id, entity + layout.subclass, sizeof(ability.subclass_id));
  std::memcpy(&ability.slot, entity + layout.slot, sizeof(ability.slot));
  std::memcpy(&ability.upgrade_info, entity + layout.upgrade_info, sizeof(ability.upgrade_info));
  std::memcpy(&ability.charges, entity + layout.charges, sizeof(ability.charges));

  // Read its timers; each must be finite.
  std::memcpy(&ability.cooldown_end, entity + layout.cooldown_end, sizeof(ability.cooldown_end));
  std::memcpy(&ability.cooldown_start, entity + layout.cooldown_start,
              sizeof(ability.cooldown_start));
  std::memcpy(&ability.charge_recharge_start, entity + layout.charge_recharge_start,
              sizeof(ability.charge_recharge_start));
  std::memcpy(&ability.charge_recharge_end, entity + layout.charge_recharge_end,
              sizeof(ability.charge_recharge_end));
  const float timers[] = {ability.cooldown_end, ability.cooldown_start,
                          ability.charge_recharge_start, ability.charge_recharge_end};
  if (!AllFinite(timers)) {
    return std::unexpected("ability timer is not finite");
  }

  // It must belong to this pawn and have a subclass.
  if (owner != pawn_handle) {
    return std::unexpected("ability owner does not match the pawn");
  }
  if (!ability.subclass_id) {
    return std::unexpected("owned ability has no subclass identity");
  }
  ReadMovementSlotState(layout, entity, ability);
  return ability;
}

}  // namespace

std::expected<std::vector<PawnObserver::Ability>, std::string>
PawnObserver::CurrentAbilitiesForSlot(int32_t slot) {
  const auto here = Locate(slot);
  if (!here) {
    return std::unexpected(here.error());
  }
  return AbilitiesOf(*here);
}

std::expected<std::vector<PawnObserver::Ability>, std::string> PawnObserver::PinnedAbilities(
    int32_t slot, const Located& before) {
  const auto here = Relocate(slot, before);
  if (!here) {
    return std::unexpected(here.error());
  }
  return AbilitiesOf(*here);
}

std::expected<std::vector<PawnObserver::Ability>, std::string> PawnObserver::AbilitiesOf(
    const Located& here) {
  // Resolve the ability layout once.
  if (!ability_layout_) {
    if (!seams_.ability_layout) {
      return std::unexpected("ability schema resolver is unavailable");
    }
    auto layout = seams_.ability_layout();
    if (!layout) {
      return std::unexpected(layout.error());
    }
    ability_layout_ = *layout;
  }
  const auto& layout = *ability_layout_;

  // Read the ability handle vector.
  const auto handles = ReadUtlVector<const uint32_t>(
      static_cast<const unsigned char*>(here.pawn) + layout.handles, kMaxAbilities);
  if (!handles) {
    return std::unexpected("invalid ability handle vector");
  }

  // Read each owned ability.
  std::vector<Ability> abilities;
  abilities.reserve(handles->count);
  for (int32_t i = 0; i < handles->count; ++i) {
    // Skip an empty handle; a live one must resolve.
    const uint32_t handle = handles->data[i];
    if (handle == kInvalidEHandleIndex) {
      continue;
    }
    auto* identity = IdentityOfHandle(here.entity_list, handle);
    if (!identity) {
      return std::unexpected("owned ability handle is unresolved");
    }
    const auto* entity = static_cast<const unsigned char*>(InstanceOf(identity));
    if (!entity) {
      return std::unexpected("owned ability has no live instance");
    }

    // Read it, and check that it belongs to this pawn.
    auto ability = ReadOwnedAbility(layout, entity, handle, here.pawn_handle);
    if (!ability) {
      return std::unexpected(ability.error());
    }

    // Keep it unless its handle repeats.
    if (std::any_of(abilities.begin(), abilities.end(),
                    [handle](const auto& a) { return a.handle == handle; })) {
      return std::unexpected("duplicate owned ability handle");
    }
    abilities.push_back(*ability);
  }
  return abilities;
}

std::expected<SetUpgradeBits, std::string> ResolveSetUpgradeBits(const ModuleImage& server) {
  auto address = ResolveSignature(server, "ability.set-upgrade-bits");
  if (!address) {
    return std::unexpected(address.error());
  }
  return reinterpret_cast<SetUpgradeBits>(*address);
}

std::expected<CreateAbility, std::string> ResolveCreateAbility(const ModuleImage& server) {
  // Resolve the creation entry, wrapped in the slot selection on Windows.
  auto address = ResolveSignature(server, "ability.create-and-register");
  if (!address) {
    return std::unexpected(address.error());
  }
#if defined(_WIN32)
  auto selection = ResolveSlotSelection(server);
  if (!selection) {
    return std::unexpected(selection.error());
  }
  const auto native = reinterpret_cast<NativeCreateAbility>(*address);
  return CreateAbility([native, owner = *selection](void* component, void* definition,
                                                    uint16_t slot, uint64_t upgrade, bool flag,
                                                    void* extra) {
    return owner->Create(native, component, definition, slot, upgrade, flag, extra);
  });
#else
  using NativeCreateAbility = void* (*)(void*, void*, uint16_t, uint64_t, bool, void*);
  return CreateAbility(reinterpret_cast<NativeCreateAbility>(*address));
#endif
}

std::expected<void, std::string> PawnObserver::ReconcileAbilities(
    int32_t slot, std::span<const AbilityUpgrade> targets, const AbilityDefinitions& definitions,
    CreateAbility create, SetUpgradeBits set_bits) {
  // Each bound slot is unique; unbound abilities are distinct by definition.
  for (size_t i = 0; i < targets.size(); ++i) {
    const auto& target = targets[i];
    // Many triggered abilities may share the unbound slot.
    const bool repeated = std::ranges::any_of(targets.first(i), [&](const auto& previous) {
      return previous.slot == target.slot &&
             (target.slot != kUnboundSlot || previous.subclass_id == target.subclass_id);
    });
    if (target.slot > kUnboundSlot || IsActiveItemSlot(target.slot) || repeated) {
      return std::unexpected("invalid or duplicate replay ability slot");
    }
  }

  // Create each target the pawn lacks, then apply the upgrades.
  const auto here = Locate(slot);
  if (!here) {
    return std::unexpected(here.error());
  }
  for (const auto& target : targets) {
    // Skip an ability already owned in its slot.
    auto owned = PinnedAbilities(slot, *here);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    const auto existing = std::ranges::find_if(*owned, [&](const auto& ability) {
      return ability.subclass_id == target.subclass_id && ability.slot == target.slot;
    });
    if (existing != owned->end()) {
      continue;
    }

    // Otherwise the slot must be free and the definition an enabled ability.
    if (target.slot != kUnboundSlot &&
        std::ranges::find(*owned, target.slot, &Ability::slot) != owned->end()) {
      return std::unexpected("replay ability slot contains another definition");
    }
    if (!create) {
      return std::unexpected("native ability creation unavailable");
    }
    const auto definition = definitions.Find(target.subclass_id);
    if (!definition) {
      return std::unexpected(definition.error());
    }
    if (definition->disabled || definition->name.starts_with("upgrade_")) {
      return std::unexpected("replay ability is not an enabled non-item definition");
    }

    // Create it and check that it landed.
    create(static_cast<unsigned char*>(here->pawn) + ability_layout_->component,
           definition->native_definition_pointer, target.slot, 0, true, nullptr);
    owned = PinnedAbilities(slot, *here);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    if (std::ranges::count_if(*owned, [&](const Ability& ability) {
          return ability.slot == target.slot && ability.subclass_id == target.subclass_id;
        }) != 1) {
      return std::unexpected(
          "native ability creation did not publish the requested owner and slot");
    }
  }
  return ApplyAbilityUpgrades(slot, targets, set_bits);
}

namespace {

// UpgradeLowWordSetter resolves the engine setter for m_nUpgradeInfo's low
// word once; builds before game build 6711 have none.
using SetUpgradeLowWord = void (*)(void* ability, uint32_t value);

SetUpgradeLowWord UpgradeLowWordSetter() {
#if defined(_WIN32)
  static const SetUpgradeLowWord setter = []() -> SetUpgradeLowWord {
    const auto server = MappedModuleImage::ForModule(L"server.dll");
    if (!server) {
      return nullptr;
    }
    const auto address = ResolveSignature(*server, "ability.set-upgrade-low-word");
    return address ? reinterpret_cast<SetUpgradeLowWord>(*address) : nullptr;
  }();
  return setter;
#else
  return nullptr;
#endif
}

}  // namespace

std::expected<void, std::string> PawnObserver::ApplyAbilityUpgrades(
    int32_t slot, std::span<const AbilityUpgrade> upgrades, SetUpgradeBits set_bits) {
  // Each upgrade must match one owned ability, and a changed low word needs its setter.
  if (!set_bits) {
    return std::unexpected("engine upgrade setter is unavailable");
  }
  const auto set_low_word = UpgradeLowWordSetter();
  const auto here = Locate(slot);
  if (!here) {
    return std::unexpected(here.error());
  }
  auto owned = AbilitiesOf(*here);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  std::vector<uint32_t> handles;
  for (const auto& target : upgrades) {
    // Each upgrade matches one owned ability, once.
    const auto matches = [&](const Ability& ability) {
      return ability.subclass_id == target.subclass_id && ability.slot == target.slot;
    };
    if (std::count_if(owned->begin(), owned->end(), matches) != 1) {
      return std::unexpected("upgrade target does not uniquely match an owned ability");
    }
    const auto& ability = *std::find_if(owned->begin(), owned->end(), matches);
    if ((ability.upgrade_info & 0xffff) != (target.upgrade_info & 0xffff) && !set_low_word) {
      return std::unexpected("upgrade packed low word differs; setter cannot restore it");
    }
    if (std::find(handles.begin(), handles.end(), ability.handle) != handles.end()) {
      return std::unexpected("duplicate ability upgrade target");
    }
    handles.push_back(ability.handle);
  }

  // Apply each upgrade.
  for (size_t i = 0; i < upgrades.size(); ++i) {
    // A setter can update other abilities. Revalidate ownership after every
    // engine call, without retaining any borrowed entity pointer across it.
    owned = PinnedAbilities(slot, *here);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    const auto& target = upgrades[i];
    const auto found = std::find_if(owned->begin(), owned->end(), [&](const Ability& ability) {
      return ability.handle == handles[i] && ability.subclass_id == target.subclass_id &&
             ability.slot == target.slot;
    });
    if (found == owned->end()) {
      return std::unexpected("upgrade target changed during apply");
    }
    if (found->upgrade_info == target.upgrade_info) {
      continue;
    }

    // Set the low word if it differs, then the bits.
    auto* identity = IdentityOfHandle(here->entity_list, found->handle);
    if (!identity) {
      return std::unexpected("upgrade target expired during apply");
    }
    if ((found->upgrade_info & 0xffff) != (target.upgrade_info & 0xffff)) {
      if (!set_low_word) {
        return std::unexpected("upgrade target changed during apply");
      }
      set_low_word(InstanceOf(identity), target.upgrade_info & 0xffff);
    }
    set_bits(InstanceOf(identity), target.upgrade_info >> 16);
  }

  // Include the final setter in the ownership check before returning success.
  owned = PinnedAbilities(slot, *here);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  return {};
}

void* PawnObserver::AbilityInstance(int32_t slot, uint32_t handle) {
  // The located pawn must own the ability.
  const auto here = Locate(slot);
  if (!here) {
    return nullptr;
  }
  const auto owned = AbilitiesOf(*here);
  if (!owned || std::none_of(owned->begin(), owned->end(),
                             [&](const auto& ability) { return ability.handle == handle; })) {
    return nullptr;
  }
  return InstanceOf(IdentityOfHandle(here->entity_list, handle));
}

std::expected<void, std::string> PawnObserver::ApplyAbilityTimers(
    int32_t slot, std::span<const Ability> targets) {
  // Match targets by identity and by timers.
  const auto here = Locate(slot);
  if (!here) {
    return std::unexpected(here.error());
  }
  auto owned = AbilitiesOf(*here);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  const auto matches = [](const Ability& actual, const Ability& target) {
    return actual.handle == target.handle && actual.subclass_id == target.subclass_id &&
           actual.slot == target.slot;
  };
  const auto timers_match = [](const Ability& actual, const Ability& target) {
    return actual.charges == target.charges && actual.cooldown_start == target.cooldown_start &&
           actual.cooldown_end == target.cooldown_end &&
           actual.charge_recharge_start == target.charge_recharge_start &&
           actual.charge_recharge_end == target.charge_recharge_end;
  };

  // Check every target before writing any.
  std::vector<uint32_t> handles;
  for (const auto& target : targets) {
    // Each target must be valid, unique, owned and replicable.
    if (target.charges < 0 || !std::isfinite(target.cooldown_start) ||
        !std::isfinite(target.cooldown_end) || !std::isfinite(target.charge_recharge_start) ||
        !std::isfinite(target.charge_recharge_end)) {
      return std::unexpected("invalid ability timer target");
    }
    if (std::find(handles.begin(), handles.end(), target.handle) != handles.end()) {
      return std::unexpected("duplicate ability timer target");
    }
    handles.push_back(target.handle);
    if (std::none_of(owned->begin(), owned->end(),
                     [&](const auto& a) { return matches(a, target); })) {
      return std::unexpected("timer target does not match an owned ability");
    }
    auto* identity = IdentityOfHandle(here->entity_list, target.handle);
    if (!CanNotifyEntityStateChanged(InstanceOf(identity))) {
      return std::unexpected("ability replication notification is unavailable");
    }
  }

  // Write each target's timers and read them back.
  for (const auto& target : targets) {
    // Skip a target whose timers already match.
    owned = PinnedAbilities(slot, *here);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    const auto found = std::find_if(owned->begin(), owned->end(),
                                    [&](const auto& a) { return matches(a, target); });
    if (found == owned->end()) {
      return std::unexpected("ability identity changed during timer restoration");
    }
    if (timers_match(*found, target)) {
      continue;
    }

    // Find the ability's entity.
    auto* identity = IdentityOfHandle(here->entity_list, target.handle);
    auto* entity = static_cast<unsigned char*>(InstanceOf(identity));
    const auto& layout = *ability_layout_;

    // Write its charges and timers and replicate them.
    std::memcpy(entity + layout.charges, &target.charges, sizeof(target.charges));
    std::memcpy(entity + layout.cooldown_start, &target.cooldown_start,
                sizeof(target.cooldown_start));
    std::memcpy(entity + layout.cooldown_end, &target.cooldown_end, sizeof(target.cooldown_end));
    std::memcpy(entity + layout.charge_recharge_start, &target.charge_recharge_start,
                sizeof(target.charge_recharge_start));
    std::memcpy(entity + layout.charge_recharge_end, &target.charge_recharge_end,
                sizeof(target.charge_recharge_end));
    if (!NotifyEntityStateChanged(entity)) {
      return std::unexpected("ability replication notification is unavailable");
    }

    // Read them back.
    owned = PinnedAbilities(slot, *here);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    const auto verified = std::find_if(owned->begin(), owned->end(),
                                       [&](const auto& a) { return matches(a, target); });
    if (verified == owned->end() || !timers_match(*verified, target)) {
      return std::unexpected("ability timer readback differs");
    }
  }

  // Confirm the batch held after the last write.
  for (const auto& target : targets) {
    if (std::none_of(owned->begin(), owned->end(), [&](const auto& actual) {
          return matches(actual, target) && timers_match(actual, target);
        })) {
      return std::unexpected("ability timers changed during batch restoration");
    }
  }
  return {};
}

std::expected<std::vector<PawnObserver::Ability>, std::string> PawnObserver::FreshAbilityTargets(
    int32_t slot) {
  // Read the owned abilities and the charge reader.
  const auto here = Locate(slot);
  if (!here) {
    return std::unexpected(here.error());
  }
  auto targets = AbilitiesOf(*here);
  if (!targets) {
    return std::unexpected(targets.error());
  }
  if (!seams_.max_charges) {
    return std::unexpected("native ability charge reader unavailable");
  }

  // Each fresh ability has full charges and no cooldown.
  for (auto& target : *targets) {
    // Check that the ability is still owned.
    auto current = PinnedAbilities(slot, *here);
    if (!current) {
      return std::unexpected(current.error());
    }
    const auto matches = [&](const Ability& value) {
      return value.handle == target.handle && value.subclass_id == target.subclass_id &&
             value.slot == target.slot;
    };
    if (std::none_of(current->begin(), current->end(), matches)) {
      return std::unexpected("ability changed before reading its charge maximum");
    }

    // Read its charge maximum and check ownership again.
    auto* identity = IdentityOfHandle(here->entity_list, target.handle);
    auto maximum = seams_.max_charges(InstanceOf(identity));
    if (!maximum) {
      return std::unexpected(maximum.error());
    }
    current = PinnedAbilities(slot, *here);
    if (!current || std::none_of(current->begin(), current->end(), matches)) {
      return std::unexpected("ability ownership changed while reading its charge maximum");
    }

    // Restore full charges and clear the timers.
    if (*maximum < 0) {
      return std::unexpected("native ability charge maximum is negative");
    }
    target.charges = *maximum;
    target.cooldown_start = target.cooldown_end = 0;
    target.charge_recharge_start = target.charge_recharge_end = 0;
  }
  return targets;
}

std::expected<void, std::string> PawnObserver::SetPracticeStamina(int32_t slot, float now,
                                                                  std::optional<float> stamina) {
  // The engine clock owns "now"; a non-finite time or value is a caller defect.
  if (!std::isfinite(now)) {
    return std::unexpected("stamina latch time is not finite");
  }
  if (stamina && !std::isfinite(*stamina)) {
    return std::unexpected("stamina is not finite");
  }

  // Locate the pawn and read its stamina resource.
  const auto here = Locate(slot);
  if (!here) {
    return std::unexpected(here.error());
  }
  if (!layout_.stamina) {
    return std::unexpected("stamina schema capability is unavailable");
  }
  const auto& st = *layout_.stamina;
  auto* pawn = static_cast<unsigned char*>(here->pawn);
  float current = 0, max = 0;
  std::memcpy(&current, pawn + st.current, sizeof(current));
  std::memcpy(&max, pawn + st.max, sizeof(max));

  // The native maximum bounds the target; it must be a real capacity.
  if (!std::isfinite(current) || !std::isfinite(max) || max <= 0) {
    return std::unexpected("native stamina resource is not usable");
  }

  // Mirror the installed HeroRefresh field relationships: current = value,
  // latchValue = value, latchTime = the caller's simulation time. Native
  // m_flPrevRegenRate is retained untouched.
  const float value = std::clamp(stamina.value_or(max), 0.0f, max);
  std::memcpy(pawn + st.current, &value, sizeof(value));
  std::memcpy(pawn + st.latch_value, &value, sizeof(value));
  std::memcpy(pawn + st.latch_time, &now, sizeof(now));
  if (!NotifyEntityStateChanged(pawn)) {
    return std::unexpected("stamina replication notification is unavailable");
  }

  // The notification callback can mutate or destroy entities. Locate the
  // same pawn again, then verify the write.
  if (const auto after = Relocate(slot, *here); !after) {
    return std::unexpected(after.error());
  }
  float written_current = 0, written_latch_value = 0, written_latch_time = 0;
  std::memcpy(&written_current, pawn + st.current, sizeof(written_current));
  std::memcpy(&written_latch_value, pawn + st.latch_value, sizeof(written_latch_value));
  std::memcpy(&written_latch_time, pawn + st.latch_time, sizeof(written_latch_time));
  if (written_current != value || written_latch_value != value || written_latch_time != now) {
    return std::unexpected("stamina readback differs after replication notification");
  }
  return {};
}

}  // namespace modlock::gameinterop
