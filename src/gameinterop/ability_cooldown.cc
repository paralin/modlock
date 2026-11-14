#include "modlock/gameinterop/ability_cooldown.h"

#include <array>
#include <cmath>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/vtable_slot_hook.h"

namespace modlock::gameinterop {

namespace {

// Lengths is one timer's query slot, the hooks on it and the seconds given.
struct Lengths {
  size_t slot = 0;

  // hooks holds one hook per dispatch table, which the abilities of a class share.
  std::unordered_map<void**, VtableSlotHook> hooks;
  std::unordered_map<uint32_t, float> seconds;
};

}  // namespace

struct AbilityCooldown::Impl {
  HandleOf handle_of = nullptr;

  // timers holds the lengths by Timer.
  std::array<Lengths, 2> timers;
};

namespace {

using LengthOf = float (*)(void*);

// active is the one live AbilityCooldown; the replacements carry no state of
// their own. It is read and written on the engine thread.
AbilityCooldown::Impl* active = nullptr;

// Answer gives how long an ability's timer lasts: the seconds given when it
// has them, otherwise whatever the table's own entry answers.
float Answer(AbilityCooldown::Timer timer, void* ability) {
  // Find the hook for the ability's table.
  const auto& lengths = active->timers[static_cast<size_t>(timer)];
  auto* table = *static_cast<void***>(ability);
  const auto hook = lengths.hooks.find(table);
  if (hook == lengths.hooks.end()) {
    return 0;
  }

  // An ability with a length reports it; others ask the original.
  if (const auto handle = active->handle_of(ability)) {
    if (const auto given = lengths.seconds.find(*handle); given != lengths.seconds.end()) {
      return given->second;
    }
  }
  return reinterpret_cast<LengthOf>(hook->second.Original())(ability);
}

// Cooldown replaces the cooldown length query.
float Cooldown(void* ability) {
  return Answer(AbilityCooldown::Timer::kCooldown, ability);
}

// ChargeDelay replaces the query for the wait between charges.
float ChargeDelay(void* ability) {
  return Answer(AbilityCooldown::Timer::kChargeDelay, ability);
}

// kCooldownCall and kChargeDelayCall are where each signature's match holds
// the virtual call's table displacement.
constexpr size_t kCooldownCall = 2;
constexpr size_t kChargeDelayCall = 12;

// SlotAt reads the dispatch slot of the virtual call a signature matched.
std::expected<size_t, std::string> SlotAt(const ModuleImage& server, std::string_view id,
                                          size_t call) {
  // Find the call.
  const auto address = ResolveSignature(server, id);
  if (!address) {
    return std::unexpected(address.error());
  }

  // Read its table displacement, which must name a whole slot.
  const auto offset = reinterpret_cast<uintptr_t>(*address) - server.base();
  uint32_t displacement = 0;
  std::memcpy(&displacement, server.image_bytes().data() + offset + call, sizeof(displacement));
  if (displacement % sizeof(void*)) {
    return std::unexpected(std::string(id) + " has an unaligned virtual slot");
  }
  return displacement / sizeof(void*);
}

}  // namespace

std::expected<AbilityCooldownSlots, std::string> ResolveAbilityCooldownSlots(
    const ModuleImage& server) {
  // The routine that starts a cooldown asks the ability for its length, and
  // an ability with charges for the wait between them.
  const auto cooldown = SlotAt(server, "ability.cooldown-length", kCooldownCall);
  if (!cooldown) {
    return std::unexpected(cooldown.error());
  }
  const auto charge_delay = SlotAt(server, "ability.charge-delay", kChargeDelayCall);
  if (!charge_delay) {
    return std::unexpected(charge_delay.error());
  }
  return AbilityCooldownSlots{*cooldown, *charge_delay};
}

std::expected<std::unique_ptr<AbilityCooldown>, std::string> AbilityCooldown::Create(
    AbilityCooldownSlots slots, HandleOf handle_of) {
  // Install one at a time.
  if (active != nullptr) {
    return std::unexpected("an ability cooldown is already installed");
  }
  auto impl = std::make_unique<Impl>();
  impl->handle_of = handle_of != nullptr ? handle_of : ReferenceHandleOf;
  impl->timers[static_cast<size_t>(Timer::kCooldown)].slot = slots.cooldown;
  impl->timers[static_cast<size_t>(Timer::kChargeDelay)].slot = slots.charge_delay;
  active = impl.get();
  return std::unique_ptr<AbilityCooldown>(new AbilityCooldown(std::move(impl)));
}

AbilityCooldown::AbilityCooldown(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AbilityCooldown::~AbilityCooldown() {
  // The hooks restore their entries as they go.
  for (auto& lengths : impl_->timers) {
    lengths.hooks.clear();
  }
  active = nullptr;
}

std::expected<void, std::string> AbilityCooldown::Set(void* ability, uint32_t handle, Timer timer,
                                                      std::optional<float> seconds) {
  // The ability must be live and the length a time.
  if (ability == nullptr) {
    return std::unexpected("the ability is not live");
  }
  if (seconds && (!std::isfinite(*seconds) || *seconds < 0)) {
    return std::unexpected("a cooldown length must be a number of seconds, 0 or more");
  }
  if (impl_->handle_of(ability) != handle) {
    return std::unexpected("the ability handle does not match its entity");
  }

  // Nothing removes the length.
  auto& lengths = impl_->timers[static_cast<size_t>(timer)];
  if (!seconds) {
    lengths.seconds.erase(handle);
    return {};
  }

  // Hook the ability's table once and store the length.
  auto* table = *static_cast<void***>(ability);
  if (!lengths.hooks.contains(table)) {
    auto hook = VtableSlotHook::Install(
        ability, lengths.slot,
        reinterpret_cast<void*>(timer == Timer::kCooldown ? &Cooldown : &ChargeDelay));
    if (!hook) {
      return std::unexpected(hook.error());
    }
    lengths.hooks.emplace(table, std::move(*hook));
  }
  lengths.seconds[handle] = *seconds;
  return {};
}

std::optional<float> AbilityCooldown::Length(uint32_t handle, Timer timer) const {
  const auto& given = impl_->timers[static_cast<size_t>(timer)].seconds;
  const auto found = given.find(handle);
  if (found == given.end()) {
    return std::nullopt;
  }
  return found->second;
}

}  // namespace modlock::gameinterop
