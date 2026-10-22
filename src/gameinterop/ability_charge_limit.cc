#include "modlock/gameinterop/ability_charge_limit.h"

#include <unordered_map>
#include <utility>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/vtable_slot_hook.h"

namespace modlock::gameinterop {

struct AbilityChargeLimit::Impl {
  size_t slot = 0;
  HandleOf handle_of = nullptr;

  // hooks holds one hook per dispatch table, which the abilities of a class share.
  std::unordered_map<void**, VtableSlotHook> hooks;
  std::unordered_map<uint32_t, int32_t> limits;
};

namespace {

using MaxCharges = int32_t (*)(void*);

// active is the one live AbilityChargeLimit; the replacement carries no state
// of its own. It is read and written on the engine thread.
AbilityChargeLimit::Impl* active = nullptr;

// Limited answers the most charges an ability holds: its limit when it has one,
// otherwise whatever the table's own entry answers.
int32_t Limited(void* ability) {
  // Find the hook for the ability's table.
  auto* table = *static_cast<void***>(ability);
  const auto hook = active->hooks.find(table);
  if (hook == active->hooks.end()) {
    return 0;
  }

  // A limited ability reports its limit; others ask the original.
  if (const auto handle = active->handle_of(ability)) {
    if (const auto limit = active->limits.find(*handle); limit != active->limits.end()) {
      return limit->second;
    }
  }
  return reinterpret_cast<MaxCharges>(hook->second.Original())(ability);
}

}  // namespace

std::expected<std::unique_ptr<AbilityChargeLimit>, std::string> AbilityChargeLimit::Create(
    size_t slot, HandleOf handle_of) {
  // Install one limit at a time.
  if (active != nullptr) {
    return std::unexpected("an ability charge limit is already installed");
  }
  auto impl = std::make_unique<Impl>();
  impl->slot = slot;
  impl->handle_of = handle_of != nullptr ? handle_of : ReferenceHandleOf;
  active = impl.get();
  return std::unique_ptr<AbilityChargeLimit>(new AbilityChargeLimit(std::move(impl)));
}

AbilityChargeLimit::AbilityChargeLimit(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AbilityChargeLimit::~AbilityChargeLimit() {
  // The hooks restore their entries as they go.
  impl_->hooks.clear();
  active = nullptr;
}

std::expected<void, std::string> AbilityChargeLimit::Set(void* ability, uint32_t handle,
                                                         int32_t maximum) {
  // The ability must be live and the maximum in range.
  if (ability == nullptr) {
    return std::unexpected("the ability is not live");
  }
  if (maximum < 0 || maximum > kMostCharges) {
    return std::unexpected("the most charges must be from 0 to " + std::to_string(kMostCharges));
  }
  if (impl_->handle_of(ability) != handle) {
    return std::unexpected("the ability handle does not match its entity");
  }

  // Zero removes the limit.
  if (maximum == 0) {
    impl_->limits.erase(handle);
    return {};
  }

  // Hook the ability's table once and store the limit.
  auto* table = *static_cast<void***>(ability);
  if (!impl_->hooks.contains(table)) {
    auto hook = VtableSlotHook::Install(ability, impl_->slot, reinterpret_cast<void*>(&Limited));
    if (!hook) {
      return std::unexpected(hook.error());
    }
    impl_->hooks.emplace(table, std::move(*hook));
  }
  impl_->limits[handle] = maximum;
  return {};
}

std::optional<int32_t> AbilityChargeLimit::Limit(uint32_t handle) const {
  const auto found = impl_->limits.find(handle);
  if (found == impl_->limits.end()) {
    return std::nullopt;
  }
  return found->second;
}

}  // namespace modlock::gameinterop
