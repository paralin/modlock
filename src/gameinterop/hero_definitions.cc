#include "modlock/gameinterop/hero_definitions.h"

#include <limits>

namespace modlock::gameinterop {
std::expected<HeroDefinitions, std::string> HeroDefinitions::Resolve(const ModuleImage& server) {
  // Resolve each call by signature.
  HeroDefinitionCalls calls;
  struct Entry {
    std::string_view id;
    void** slot;
  };
  const Entry entries[] = {
      {"hero-definition-manager.get-manager", reinterpret_cast<void**>(&calls.manager_getter)},
      {"hero-definition-manager.hero-name-to-id", reinterpret_cast<void**>(&calls.hero_name_to_id)},
      {"hero-definition-manager.get-hero-by-id", reinterpret_cast<void**>(&calls.get_hero_by_id)},
  };
  for (const auto& entry : entries) {
    auto address = ResolveSignature(server, entry.id);
    if (!address) {
      return std::unexpected(address.error());
    }
    *entry.slot = *address;
  }
  return HeroDefinitions(calls);
}

HeroDefinitions::HeroDefinitions(HeroDefinitionCalls calls) : calls_(calls) {}

std::expected<HeroDefinition, std::string> HeroDefinitions::Find(std::string_view hero_name) const {
  // Fail closed before any engine call: a caller-built owner (tests,
  // alternate constructors) may carry unresolved slots.
  if (calls_.manager_getter == nullptr) {
    return std::unexpected("hero definition calls: manager getter is unresolved");
  }
  if (calls_.hero_name_to_id == nullptr) {
    return std::unexpected("hero definition calls: hero-name-to-id is unresolved");
  }
  if (calls_.get_hero_by_id == nullptr) {
    return std::unexpected("hero definition calls: get-hero-by-id is unresolved");
  }

  // The manager is re-resolved on every call: it is TLS-guarded and dies
  // with the world that created it.
  void* manager = calls_.manager_getter();
  if (manager == nullptr) {
    return std::unexpected("hero definition manager is not live yet");
  }

  // HeroNameToId returns a pointer; success is judged
  // from an initialized out_id alone. Never dereference or log the returned
  // pointer.
  int hero_id = -1;
  std::string name(hero_name);
  calls_.hero_name_to_id(manager, &hero_id, name.c_str());
  if (hero_id < 0) {
    return std::unexpected("unknown hero name '" + name + "'");
  }
  return FindInManager(manager, static_cast<uint32_t>(hero_id));
}

std::expected<HeroDefinition, std::string> HeroDefinitions::Find(uint32_t hero_id) const {
  // The id must fit and the calls must resolve.
  if (hero_id > std::numeric_limits<int>::max()) {
    return std::unexpected("hero id is outside the native identifier range");
  }
  if (calls_.manager_getter == nullptr) {
    return std::unexpected("hero definition calls: manager getter is unresolved");
  }
  if (calls_.get_hero_by_id == nullptr) {
    return std::unexpected("hero definition calls: get-hero-by-id is unresolved");
  }

  // Look the id up in the live manager.
  void* manager = calls_.manager_getter();
  if (manager == nullptr) {
    return std::unexpected("hero definition manager is not live yet");
  }
  return FindInManager(manager, hero_id);
}

std::expected<HeroDefinition, std::string> HeroDefinitions::FindInManager(void* manager,
                                                                          uint32_t hero_id) const {
  void* hero_def = calls_.get_hero_by_id(manager, hero_id);
  if (hero_def == nullptr) {
    return std::unexpected("hero id " + std::to_string(hero_id) + " resolved to no definition");
  }
  return HeroDefinition{.id = static_cast<int>(hero_id), .native_definition_pointer = hero_def};
}

}  // namespace modlock::gameinterop
