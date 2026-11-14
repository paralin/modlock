#include "modlock/gameinterop/ability_cooldown.h"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/vtable_slot_hook.h"

namespace modlock::gameinterop {

struct AbilityCooldown::Impl {
  HandleOf handle_of = nullptr;
  size_t slot = 0;

  // hooks holds one hook per dispatch table, which the abilities of a class
  // share, and seconds the length given each ability by handle.
  std::unordered_map<void**, VtableSlotHook> hooks;
  std::unordered_map<uint32_t, float> seconds;
};

namespace {

using LengthOf = float (*)(void*);

// active is the one live AbilityCooldown; the replacement carries no state of
// its own. It is read and written on the engine thread.
AbilityCooldown::Impl* active = nullptr;

// Cooldown replaces the cooldown length query: the seconds given when the
// ability has them, otherwise whatever the table's own entry answers.
float Cooldown(void* ability) {
  // Find the hook for the ability's table.
  auto* table = *static_cast<void***>(ability);
  const auto hook = active->hooks.find(table);
  if (hook == active->hooks.end()) {
    return 0;
  }

  // An ability with a length reports it; others ask the original.
  if (const auto handle = active->handle_of(ability)) {
    if (const auto given = active->seconds.find(*handle); given != active->seconds.end()) {
      return given->second;
    }
  }
  return reinterpret_cast<LengthOf>(hook->second.Original())(ability);
}

// kCooldownCall is where the signature's match holds the virtual call's
// table displacement.
constexpr size_t kCooldownCall = 2;

}  // namespace

std::expected<size_t, std::string> ResolveAbilityCooldownSlot(const ModuleImage& server) {
  // Find the call the routine that starts a cooldown makes for its length.
  const auto address = ResolveSignature(server, "ability.cooldown-length");
  if (!address) {
    return std::unexpected(address.error());
  }

  // Read its table displacement, which must name a whole slot.
  const auto offset = reinterpret_cast<uintptr_t>(*address) - server.base();
  uint32_t displacement = 0;
  std::memcpy(&displacement, server.image_bytes().data() + offset + kCooldownCall,
              sizeof(displacement));
  if (displacement % sizeof(void*)) {
    return std::unexpected("ability.cooldown-length has an unaligned virtual slot");
  }
  return displacement / sizeof(void*);
}

std::expected<std::unique_ptr<AbilityCooldown>, std::string> AbilityCooldown::Create(
    size_t slot, HandleOf handle_of) {
  // Install one at a time.
  if (active != nullptr) {
    return std::unexpected("an ability cooldown is already installed");
  }
  auto impl = std::make_unique<Impl>();
  impl->handle_of = handle_of != nullptr ? handle_of : ReferenceHandleOf;
  impl->slot = slot;
  active = impl.get();
  return std::unique_ptr<AbilityCooldown>(new AbilityCooldown(std::move(impl)));
}

AbilityCooldown::AbilityCooldown(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AbilityCooldown::~AbilityCooldown() {
  // The hooks restore their entries as they go.
  impl_->hooks.clear();
  active = nullptr;
}

std::expected<void, std::string> AbilityCooldown::Set(void* ability, uint32_t handle,
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

  // No value returns the ability to the game's own length.
  if (!seconds) {
    impl_->seconds.erase(handle);
    return {};
  }

  // Hook the ability's table once and store the length.
  auto* table = *static_cast<void***>(ability);
  if (!impl_->hooks.contains(table)) {
    auto hook = VtableSlotHook::Install(ability, impl_->slot, reinterpret_cast<void*>(&Cooldown));
    if (!hook) {
      return std::unexpected(hook.error());
    }
    impl_->hooks.emplace(table, std::move(*hook));
  }
  impl_->seconds[handle] = *seconds;
  return {};
}

std::optional<float> AbilityCooldown::Length(uint32_t handle) const {
  const auto found = impl_->seconds.find(handle);
  if (found == impl_->seconds.end()) {
    return std::nullopt;
  }
  return found->second;
}

}  // namespace modlock::gameinterop
