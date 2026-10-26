#include <algorithm>
#include <cstring>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "pawn_observer_internal.h"

namespace modlock::gameinterop {

std::expected<TeleportClientCamera, std::string> ResolveTeleportClientCamera(
    const ModuleImage& server) {
  // The first receiver is unused. The helper teleports its second argument,
  // then sends user message 321 with all three camera axes to that pawn's client.
  auto address = ResolveSignature(server, "pawn.teleport-client-camera");
  if (!address) {
    return std::unexpected(address.error());
  }
  return reinterpret_cast<TeleportClientCamera>(*address);
}

std::expected<PawnMotion, std::string> ResolvePawnMotion(const ModuleImage& server) {
  // Resolve the three setters, or fail at the first missing one.
  PawnMotion motion{};
  const SignatureCall signatures[] = {
      {"entity.set-abs-origin", &motion.set_origin},
      {"entity.set-abs-angles", &motion.set_angles},
      {"entity.set-abs-velocity", &motion.set_velocity},
  };
  auto resolved = ResolveSignatures(server, signatures);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  return motion;
}

std::expected<ItemFunctions, std::string> ItemFunctions::Resolve(const ModuleImage& server) {
  // Resolve the item add, remove and slot swap functions.
  ItemFunctions functions{};
  const SignatureCall signatures[] = {
      {"pawn.add-item", &functions.add},
      {"ability.remove-item", &functions.remove},
      {"ability.swap-item-slots", &functions.swap_slots},
  };
  auto resolved = ResolveSignatures(server, signatures);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  // Resolve the upgrade bits setter.
  auto bits = ResolveSetUpgradeBits(server);
  if (!bits) {
    return std::unexpected(bits.error());
  }
  functions.set_bits = *bits;
  return functions;
}

std::expected<PawnObserver::Ability, std::string> PawnObserver::GrantItem(
    int32_t slot, uint32_t subclass_id, const AbilityDefinitions& definitions,
    const ItemFunctions& functions) {
  // Grant only an enabled item the pawn does not own.
  const auto definition = definitions.Find(subclass_id);
  if (!definition) {
    return std::unexpected(definition.error());
  }
  if (definition->disabled || !definition->name.starts_with("upgrade_") || !functions.add) {
    return std::unexpected("enabled native item and grant function required");
  }

  // Return the item when the located pawn already owns it.
  const auto here = Locate(slot);
  if (!here) {
    return std::unexpected(here.error());
  }
  const auto before = AbilitiesOf(*here);
  if (!before) {
    return std::unexpected(before.error());
  }
  const auto existing = std::ranges::find(*before, subclass_id, &Ability::subclass_id);
  if (existing != before->end()) {
    return *existing;
  }

  // The engine owns the returned pointer. Only the subsequent owned-ability
  // observation establishes the grant and its actual inventory slot.
  functions.add(here->pawn, definition->name.c_str(), 0, nullptr);
  const auto after = PinnedAbilities(slot, *here);
  if (!after) {
    return std::unexpected(after.error());
  }
  for (const auto& previous : *before) {
    const auto retained = std::ranges::find(*after, previous.handle, &Ability::handle);
    if (retained == after->end() || retained->subclass_id != previous.subclass_id ||
        retained->slot != previous.slot) {
      return std::unexpected("native grant changed an existing ability or item slot");
    }
  }
  const auto granted = std::ranges::find(*after, subclass_id, &Ability::subclass_id);
  if (granted == after->end() || after->size() != before->size() + 1) {
    return std::unexpected("native grant did not produce one owned item");
  }
  return *granted;
}

std::expected<void, std::string> PawnObserver::ReconcileItems(int32_t slot,
                                                              std::span<const ItemTarget> targets,
                                                              const AbilityDefinitions& definitions,
                                                              const ItemFunctions& functions) {
  // Check the functions and every target's definition and slot.
  if (!functions.add || !functions.remove || !functions.set_bits) {
    return std::unexpected("engine item functions are unavailable");
  }
  if (targets.size() > 64) {
    return std::unexpected("too many replay items");
  }
  std::vector<std::string> names;
  std::vector<uint16_t> active_slots;
  for (const auto& target : targets) {
    // Each item must be an enabled, unique upgrade with a valid slot.
    auto definition = definitions.Find(target.subclass_id);
    if (!definition) {
      return std::unexpected(definition.error());
    }
    if (definition->disabled || !definition->name.starts_with("upgrade_")) {
      return std::unexpected("replay item is not an enabled upgrade definition");
    }
    if (std::find(names.begin(), names.end(), definition->name) != names.end()) {
      return std::unexpected("duplicate replay item definition");
    }
    if (target.slot && *target.slot != kUnboundSlot) {
      if (!IsActiveItemSlot(*target.slot)) {
        return std::unexpected("invalid replay item slot");
      }
      if (!functions.swap_slots) {
        return std::unexpected("engine item slot swap is unavailable");
      }
      if (std::find(active_slots.begin(), active_slots.end(), *target.slot) != active_slots.end()) {
        return std::unexpected("duplicate replay active item slot");
      }
      active_slots.push_back(*target.slot);
    }
    names.push_back(std::move(definition->name));
  }

  // Read the located pawn's items.
  const auto here = Locate(slot);
  if (!here) {
    return std::unexpected(here.error());
  }
  const auto read_items = [&]() -> std::expected<std::vector<Ability>, std::string> {
    // Keep the owned abilities that are items.
    auto owned = PinnedAbilities(slot, *here);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    std::vector<Ability> items;
    for (const auto& ability : *owned) {
      auto definition = definitions.Find(ability.subclass_id);
      if (!definition) {
        return std::unexpected(definition.error());
      }
      if (definition->name.starts_with("upgrade_")) {
        items.push_back(ability);
      }
    }
    return items;
  };
  auto items = read_items();
  if (!items) {
    return std::unexpected(items.error());
  }

  // Remove each item no target keeps, and each whose low word differs.
  std::vector<uint32_t> kept;
  std::vector<Ability> removals;
  for (const auto& item : *items) {
    const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& t) {
      return t.subclass_id == item.subclass_id;
    });
    if (target == targets.end() ||
        std::find(kept.begin(), kept.end(), item.subclass_id) != kept.end()) {
      removals.push_back(item);
    } else if ((item.upgrade_info & 0xffff) != (target->upgrade_info & 0xffff)) {
      // Only a fresh grant sets the packed low word; replace the item.
      removals.push_back(item);
    } else {
      kept.push_back(item.subclass_id);
    }
  }
  for (const auto& removal : removals) {
    // Find the item again.
    items = read_items();
    if (!items) {
      return std::unexpected(items.error());
    }
    const auto found = std::find_if(items->begin(), items->end(), [&](const auto& item) {
      return item.handle == removal.handle && item.subclass_id == removal.subclass_id;
    });
    if (found == items->end()) {
      return std::unexpected("item changed before removal");
    }
    auto* identity = IdentityOfHandle(here->entity_list, found->handle);
    if (!identity) {
      return std::unexpected("item expired before removal");
    }

    // Remove it and check that it left.
    functions.remove(static_cast<unsigned char*>(here->pawn) + ability_layout_->component,
                     InstanceOf(identity), 0);
    items = read_items();
    if (!items) {
      return std::unexpected(items.error());
    }
    if (std::any_of(items->begin(), items->end(),
                    [&](const auto& item) { return item.handle == removal.handle; })) {
      return std::unexpected("engine item removal did not change the loadout");
    }
  }

  // Grant each missing target and apply its upgrades.
  for (size_t i = 0; i < targets.size(); ++i) {
    // Grant the item unless it is owned.
    items = read_items();
    if (!items) {
      return std::unexpected(items.error());
    }
    const auto matches = [&](const auto& item) {
      return item.subclass_id == targets[i].subclass_id;
    };
    if (std::none_of(items->begin(), items->end(), matches)) {
      // The returned pointer is borrowed engine storage. Ownership readback,
      // rather than that pointer, determines whether the grant took effect.
      // The upgrade value's low word holds the initial upgrade bits and its
      // high dword the packed low word of m_nUpgradeInfo.
      const uint64_t upgrade =
          (uint64_t{targets[i].upgrade_info & 0xffffu} << 32) | (targets[i].upgrade_info >> 16);
      functions.add(here->pawn, names[i].c_str(), upgrade, nullptr);
      items = read_items();
      if (!items) {
        return std::unexpected(items.error());
      }
    }

    // Then apply its upgrades.
    if (std::count_if(items->begin(), items->end(), matches) != 1) {
      return std::unexpected("engine item grant did not produce one owned item");
    }
    const auto& item = *std::find_if(items->begin(), items->end(), matches);
    const AbilityUpgrade upgrade{item.subclass_id, item.slot, targets[i].upgrade_info};
    auto applied = ApplyAbilityUpgrades(slot, std::span(&upgrade, 1), functions.set_bits);
    if (!applied) {
      return applied;
    }
  }

  // Move each item to its target slot.
  for (const auto& target : targets) {
    // Find each slotted item.
    if (!target.slot) {
      continue;
    }
    items = read_items();
    if (!items) {
      return std::unexpected(items.error());
    }
    const auto found = std::find_if(items->begin(), items->end(), [&](const auto& item) {
      return item.subclass_id == target.subclass_id;
    });
    if (found == items->end()) {
      return std::unexpected("item disappeared before slot restoration");
    }

    // Swap it into its slot unless it is there.
    if (found->slot == *target.slot) {
      continue;
    }
    if (!IsActiveItemSlot(found->slot) || *target.slot == kUnboundSlot) {
      return std::unexpected("item active/passive slot class differs from replay");
    }
    const auto handle = found->handle;
    functions.swap_slots(static_cast<unsigned char*>(here->pawn) + ability_layout_->component,
                         found->slot, *target.slot);

    // Check that the swap held.
    items = read_items();
    if (!items) {
      return std::unexpected(items.error());
    }
    if (std::none_of(items->begin(), items->end(), [&](const auto& item) {
          return item.handle == handle && item.subclass_id == target.subclass_id &&
                 item.slot == *target.slot;
        })) {
      return std::unexpected("engine item slot swap did not persist");
    }
  }

  // Confirm the final loadout.
  items = read_items();
  if (!items) {
    return std::unexpected(items.error());
  }
  if (items->size() != targets.size()) {
    return std::unexpected("restored item count differs");
  }
  for (const auto& target : targets) {
    if (std::count_if(items->begin(), items->end(), [&](const auto& item) {
          return item.subclass_id == target.subclass_id &&
                 item.upgrade_info == target.upgrade_info &&
                 (!target.slot || item.slot == *target.slot);
        }) != 1) {
      return std::unexpected("restored item identity or upgrade readback differs");
    }
  }
  return {};
}

}  // namespace modlock::gameinterop
