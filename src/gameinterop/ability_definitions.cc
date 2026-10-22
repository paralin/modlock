#include "modlock/gameinterop/ability_definitions.h"

#include <cstring>

namespace modlock::gameinterop {

std::expected<AbilityDefinitions, std::string> AbilityDefinitions::Resolve(
    const ModuleImage& server) {
  // LookupVDataByHash scope 4 selects abilities and items. The
  // Windows implementation consumes EDX as a 32-bit hash, including its high bit.
  auto address = ResolveSignature(server, "vdata.lookup-by-hash");
  if (!address) {
    return std::unexpected(address.error());
  }
  return AbilityDefinitions(reinterpret_cast<Lookup>(*address));
}

std::expected<AbilityDefinitions::Definition, std::string> AbilityDefinitions::Find(
    uint32_t subclass_id) const {
  // The lookup must hold the subclass.
  if (!lookup_ || !subclass_id) {
    return std::unexpected("ability definition lookup is unavailable");
  }
  auto* definition = static_cast<unsigned char*>(lookup_(4, subclass_id));
  if (!definition) {
    return std::unexpected("ability subclass is absent from the current engine");
  }

  // CEntitySubclassVDataBase: scope at +8, interned name at +16 and disabled
  // flag at +0x2a. The lookup itself checks scope; repeat it before reading the
  // SDK header so a mismatched registry result is never treated as an ability.
  int32_t scope = 0;
  const char* name = nullptr;
  std::memcpy(&scope, definition + 8, sizeof(scope));
  std::memcpy(&name, definition + 16, sizeof(name));
  if (scope != 4 || !name) {
    return std::unexpected("ability definition header is invalid");
  }
  std::string copied;
  for (size_t i = 0; i < 256; ++i) {
    const char c = name[i];
    if (!c) {
      if (copied.empty()) {
        return std::unexpected("ability definition name is empty");
      }
      return Definition{std::move(copied), definition[0x2a] != 0, definition};
    }
    // Definition identifiers are not display text or console command strings.
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '/' || c == '-')) {
      return std::unexpected("ability definition name is not an identifier");
    }
    copied.push_back(c);
  }
  return std::unexpected("ability definition name exceeds 255 bytes");
}

}  // namespace modlock::gameinterop
