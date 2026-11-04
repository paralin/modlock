#include "modlock/gameinterop/game_symbols.h"

#include <algorithm>
#include <cstring>
#include <optional>

#include "modlock/gameinterop/signature.h"

namespace modlock::gameinterop {
namespace {

// UniqueOffset returns the offset of the pattern's single match in image.
std::expected<size_t, std::string> UniqueOffset(const ModuleImage& image,
                                                const GameSignature& signature) {
  // Parse the pattern.
  const std::string id(signature.id);
  auto parsed = ParseSignature(id, signature.pattern);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  if (parsed->bytes.empty()) {
    return std::unexpected("signature '" + id + "' has no pattern");
  }

  // The pattern must match exactly once.
  const auto hits = SignatureScan(image.image_bytes(), *parsed);
  if (hits.empty()) {
    return std::unexpected("signature '" + id + "' not found in the module image");
  }
  if (hits.size() > 1) {
    return std::unexpected("signature '" + id + "' matched " + std::to_string(hits.size()) +
                           " times in the module image");
  }
  return hits.front();
}

// IsRipRelativeLoad reports whether the seven bytes at site are a REX.W LEA
// or MOV whose ModRM selects [rip + rel32].
bool IsRipRelativeLoad(std::span<const uint8_t> bytes, size_t site) {
  const uint8_t rex = bytes[site];
  const uint8_t opcode = bytes[site + 1];
  const uint8_t modrm = bytes[site + 2];
  return (rex & 0xF8) == 0x48 && (opcode == 0x8D || opcode == 0x8B) && (modrm & 0xC7) == 0x05;
}

}  // namespace

std::string_view GameModuleFile(GameModule module) {
  switch (module) {
    case GameModule::kServer:
      return "server.dll";
    case GameModule::kEngine:
      return "engine2.dll";
  }
  return {};
}

const GameSignature* FindGameSignature(std::string_view id) {
  const auto signatures = GameSignatures();
  const auto found = std::ranges::find(signatures, id, &GameSignature::id);
  return found == signatures.end() ? nullptr : &*found;
}

std::expected<void*, std::string> ResolveSignature(const ModuleImage& image,
                                                   const GameSignature& signature) {
  // A match target is the match itself.
  const auto offset = UniqueOffset(image, signature);
  if (!offset) {
    return std::unexpected(offset.error());
  }
  if (signature.target == SignatureTarget::kMatch) {
    return reinterpret_cast<void*>(image.base() + *offset);
  }

  // Validate the complete instruction before adding the delta, so even a
  // wrapping delta cannot escape the image.
  const auto bytes = image.image_bytes();
  const size_t length = signature.target == SignatureTarget::kCall ? 5 : 7;
  const size_t remaining = bytes.size() - *offset;
  if (signature.delta > remaining || remaining - signature.delta < length) {
    return std::unexpected("signature '" + std::string(signature.id) +
                           "' instruction truncated by the image boundary");
  }

  // The site must hold the expected instruction.
  const size_t site = *offset + signature.delta;
  if (signature.target == SignatureTarget::kCall && bytes[site] != 0xE8) {
    return std::unexpected("signature '" + std::string(signature.id) +
                           "' does not reach a relative call");
  }
  if (signature.target == SignatureTarget::kRipRelative && !IsRipRelativeLoad(bytes, site)) {
    return std::unexpected("signature '" + std::string(signature.id) +
                           "' does not reach a RIP-relative LEA or MOV");
  }

  // Read the displacement.
  std::int32_t rel32 = 0;
  std::memcpy(&rel32, bytes.data() + site + length - sizeof(rel32), sizeof(rel32));

  // A stale delta can still decode as some instruction; a target outside the
  // image is never the recorded function or global.
  const std::int64_t target = static_cast<std::int64_t>(site + length) + rel32;
  if (target < 0 || static_cast<std::uint64_t>(target) >= bytes.size()) {
    return std::unexpected("signature '" + std::string(signature.id) +
                           "' resolves outside the module image");
  }
  return reinterpret_cast<void*>(image.base() + static_cast<std::uintptr_t>(target));
}

std::expected<void*, std::string> ResolveSignature(const ModuleImage& image, std::string_view id) {
  const auto* signature = FindGameSignature(id);
  if (signature == nullptr) {
    return std::unexpected("signature '" + std::string(id) + "' is not recorded");
  }
  return ResolveSignature(image, *signature);
}

std::expected<void, std::string> ResolveSignatures(const ModuleImage& image,
                                                   std::span<const SignatureCall> calls) {
  for (const auto& call : calls) {
    auto address = ResolveSignature(image, call.id);
    if (!address) {
      return std::unexpected(address.error());
    }
    *call.target = *address;
  }
  return {};
}

std::expected<uint32_t, std::string> EnumeratorValue(const ModuleImage& image,
                                                     std::string_view name, int64_t limit) {
  // View the image as text.
  const auto bytes = image.image_bytes();
  const std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());

  // The name is a whole NUL-terminated string, not a prefix of a longer one.
  std::string needle;
  needle.reserve(name.size() + 2);
  needle.push_back('\0');
  needle.append(name);
  needle.push_back('\0');
  const auto at = view.find(needle);
  if (at == std::string_view::npos) {
    return std::unexpected(std::string(name) + " is not in the game module");
  }

  // The enumerator records point at the string's address.
  const uint64_t address = static_cast<uint64_t>(image.base()) + at + 1;
  char pointer[sizeof(address)];
  std::memcpy(pointer, &address, sizeof(address));
  const std::string_view pointer_view(pointer, sizeof(pointer));

  // Every enumerator record naming this string must agree on one value.
  std::optional<int64_t> value;
  for (auto hit = view.find(pointer_view); hit != std::string_view::npos;
       hit = view.find(pointer_view, hit + 1)) {
    // Each aligned record holds the string's address, then its value.
    if (hit % alignof(uint64_t) != 0 || hit + 2 * sizeof(uint64_t) > view.size()) {
      continue;
    }
    int64_t candidate = 0;
    std::memcpy(&candidate, view.data() + hit + sizeof(uint64_t), sizeof(candidate));
    if (candidate < 0 || candidate >= limit) {
      continue;
    }
    if (value && *value != candidate) {
      return std::unexpected(std::string(name) + " has conflicting values");
    }
    value = candidate;
  }
  if (!value) {
    return std::unexpected(std::string(name) + " has no enumerator record");
  }
  return static_cast<uint32_t>(*value);
}

}  // namespace modlock::gameinterop
