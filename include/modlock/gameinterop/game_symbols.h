#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/export.h"

namespace modlock::gameinterop {

// ModuleImage is one game module mapped into this process. Implementations
// supply the load base and the mapped image bytes; signature scans run over
// the mapped bytes and hits convert to absolute addresses through the base.
class MODLOCK_API ModuleImage {
 public:
  virtual ~ModuleImage() = default;

  // base returns the runtime address of byte zero. The module outlives the view.
  virtual std::uintptr_t base() const = 0;

  // image_bytes returns the borrowed mapped bytes available to the scanner.
  virtual std::span<const uint8_t> image_bytes() const = 0;
};

// GameModule is one game binary that carries scanned code. The values are
// bits so one signature can name every module that shares a function.
enum class GameModule : uint8_t {
  kServer = 1 << 0,
  kClient = 1 << 1,
  kEngine = 1 << 2,
};

constexpr uint8_t operator|(GameModule a, GameModule b) {
  return static_cast<uint8_t>(a) | static_cast<uint8_t>(b);
}

// GameModuleFile returns the module's file name, such as "server.dll".
[[nodiscard]] MODLOCK_API std::string_view GameModuleFile(GameModule module);

// SignatureTarget says which address a signature's unique match resolves to.
enum class SignatureTarget : uint8_t {
  // kMatch is the first matched byte, usually a function entry.
  kMatch,
  // kCall is the target of the E8 rel32 call at delta bytes into the match:
  // match + delta + 5 + rel32.
  kCall,
  // kRipRelative is the address operand of the seven-byte REX.W instruction
  // (LEA or MOV) at delta bytes into the match, with rel32 at delta + 3:
  // match + delta + 7 + rel32.
  kRipRelative,
};

// GameSignature is one recorded byte pattern and the address it resolves to.
// Patterns use the ParseSignature syntax; "??" marks a wildcard byte. A
// pattern must match exactly once in each module it names.
struct GameSignature {
  // id is the stable name callers resolve.
  std::string_view id;
  // modules is the set of GameModule bits whose image carries the pattern.
  uint8_t modules = static_cast<uint8_t>(GameModule::kServer);
  // pattern is the byte signature for the current game build.
  std::string_view pattern;
  // target selects the match itself or a relative operand inside it.
  SignatureTarget target = SignatureTarget::kMatch;
  // delta is the instruction offset for kCall and kRipRelative.
  size_t delta = 0;
  // shape describes the native function or instruction that was matched.
  std::string_view shape;

  // In reports whether module carries this signature.
  constexpr bool In(GameModule module) const {
    return (modules & static_cast<uint8_t>(module)) != 0;
  }
};

// GameSignatures returns every recorded signature, sorted by id. The game
// data file data/game_signatures.txtpb is the one place to change patterns
// after a game update; the build carries a copy, which LoadGameSignatures
// replaces with the file a release ships.
[[nodiscard]] MODLOCK_API std::span<const GameSignature> GameSignatures();

// GameSignatureLoad reports a game data file that replaced the recorded
// signatures.
struct GameSignatureLoad {
  // loaded counts the signatures now recorded.
  size_t loaded = 0;
  // skipped explains each entry left out, naming its id when it has one.
  std::vector<std::string> skipped;
};

// LoadGameSignatures replaces the recorded signatures with the game data
// file at path. A malformed entry is skipped, so only the features that
// resolve its id fail; an unreadable or unparsable file is an error and keeps
// the current signatures. Call it before any lookup: replacing the table
// invalidates every earlier GameSignatures span and FindGameSignature
// pointer, and it must not run concurrently with lookups.
[[nodiscard]] MODLOCK_API std::expected<GameSignatureLoad, std::string> LoadGameSignatures(
    const std::filesystem::path& path);

// FindGameSignature returns the recorded signature with id, or nullptr.
[[nodiscard]] MODLOCK_API const GameSignature* FindGameSignature(std::string_view id);

// ResolveSignature scans image for signature's pattern and resolves its
// target. Zero or multiple matches, a relative instruction truncated by the
// image, and a kCall site without E8 are errors naming the id.
[[nodiscard]] MODLOCK_API std::expected<void*, std::string> ResolveSignature(
    const ModuleImage& image, const GameSignature& signature);

// ResolveSignature resolves the recorded signature with id. An unknown id is
// an error.
[[nodiscard]] MODLOCK_API std::expected<void*, std::string> ResolveSignature(
    const ModuleImage& image, std::string_view id);

}  // namespace modlock::gameinterop
