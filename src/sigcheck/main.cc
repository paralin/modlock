// modlock-sigcheck resolves every recorded game signature against the game
// binaries on disk. Run it after a game update: each entry prints its module,
// resolved RVA, or the reason it failed. With --baseline pointing at a copy of
// the previous build, a failed entry also gets a suggested replacement
// pattern: the old match is decoded, its relative operands are wildcarded,
// and the shortest instruction-aligned prefix that matches once in the new
// build is printed. With --signatures, it checks that game data file in place
// of the signatures it was built with, so a repaired file is checked before
// it ships.
//
//   modlock-sigcheck <deadlock-dir> [--baseline <old-deadlock-dir>]
//                    [--signatures <game_signatures.txtpb>]
//
// Exit status is zero only when every entry loads and resolves.
#include <Zydis.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/signature.h"

namespace {

namespace fs = std::filesystem;
using modlock::gameinterop::GameModule;
using modlock::gameinterop::GameSignature;
using modlock::gameinterop::ModuleImage;
using modlock::gameinterop::SignatureTarget;

// kSearchSlack is how many bytes past the entry's required footprint are
// decoded when a replacement pattern is suggested.
constexpr size_t kSearchSlack = 96;

// PeImage is one PE file laid out as the loader maps it: every section is
// copied to its virtual address, so scan offsets are RVAs. base() is zero.
class PeImage final : public ModuleImage {
 public:
  static std::expected<PeImage, std::string> Load(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected("cannot open " + path.string());
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(in), {}};
    auto read32 = [&file](size_t at) -> std::optional<uint32_t> {
      if (at + 4 > file.size()) return std::nullopt;
      uint32_t value = 0;
      std::memcpy(&value, file.data() + at, 4);
      return value;
    };
    auto read16 = [&file](size_t at) -> std::optional<uint16_t> {
      if (at + 2 > file.size()) return std::nullopt;
      uint16_t value = 0;
      std::memcpy(&value, file.data() + at, 2);
      return value;
    };

    const auto pe = read32(0x3C);
    if (!pe || read32(*pe) != 0x00004550u) return std::unexpected(path.string() + " is not a PE");
    const size_t coff = *pe + 4;
    const auto section_count = read16(coff + 2);
    const auto optional_size = read16(coff + 16);
    const size_t optional = coff + 20;
    const auto image_size = read32(optional + 56);
    const auto header_size = read32(optional + 60);
    if (!section_count || !optional_size || !image_size || !header_size) {
      return std::unexpected(path.string() + " has a truncated PE header");
    }

    PeImage image;
    image.bytes_.assign(*image_size, 0);
    std::memcpy(image.bytes_.data(), file.data(),
                std::min<size_t>({*header_size, file.size(), image.bytes_.size()}));
    const size_t sections = optional + *optional_size;
    for (size_t i = 0; i < *section_count; ++i) {
      const size_t header = sections + i * 40;
      const auto virtual_address = read32(header + 12);
      const auto raw_size = read32(header + 16);
      const auto raw_offset = read32(header + 20);
      if (!virtual_address || !raw_size || !raw_offset) {
        return std::unexpected(path.string() + " has a truncated section table");
      }
      if (*raw_offset >= file.size() || *virtual_address >= image.bytes_.size()) continue;
      const size_t count = std::min<size_t>(
          {*raw_size, file.size() - *raw_offset, image.bytes_.size() - *virtual_address});
      std::memcpy(image.bytes_.data() + *virtual_address, file.data() + *raw_offset, count);
    }
    return image;
  }

  std::uintptr_t base() const override { return 0; }
  std::span<const uint8_t> image_bytes() const override { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
};

// ModulePath returns where module lives under a Deadlock install.
fs::path ModulePath(const fs::path& game, GameModule module) {
  const auto file = std::string(modlock::gameinterop::GameModuleFile(module));
  if (module == GameModule::kEngine) return game / "game" / "bin" / "win64" / file;
  return game / "game" / "citadel" / "bin" / "win64" / file;
}

// BuildVersion reads the ClientVersion line of the install's steam.inf.
std::string BuildVersion(const fs::path& game) {
  std::ifstream in(game / "game" / "citadel" / "steam.inf");
  std::string line;
  while (std::getline(in, line)) {
    if (line.starts_with("ClientVersion=")) return line.substr(14);
  }
  return "unknown";
}

// Hits counts the pattern's matches in image.
size_t Hits(const ModuleImage& image, std::string_view pattern) {
  auto parsed = modlock::gameinterop::ParseSignature("hits", pattern);
  if (!parsed || parsed->bytes.empty()) return 0;
  return modlock::gameinterop::SignatureScan(image.image_bytes(), *parsed).size();
}

// Pattern renders bytes in the signature syntax, "??" for wildcards.
std::string Pattern(std::span<const uint8_t> bytes, const std::vector<bool>& wildcard) {
  std::string out;
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (!out.empty()) out += ' ';
    if (wildcard[i]) {
      out += "??";
      continue;
    }
    char hex[3];
    std::snprintf(hex, sizeof(hex), "%02X", bytes[i]);
    out += hex;
  }
  return out;
}

// Suggest derives a replacement for signature from its unique match in old:
// relative displacements and branch targets become wildcards, and the
// shortest instruction-aligned prefix that resolves once in current wins.
std::optional<std::string> Suggest(const GameSignature& signature, const PeImage& old,
                                   const PeImage& current) {
  auto parsed = modlock::gameinterop::ParseSignature(std::string(signature.id), signature.pattern);
  if (!parsed) return std::nullopt;
  const auto hits = modlock::gameinterop::SignatureScan(old.image_bytes(), *parsed);
  if (hits.size() != 1) return std::nullopt;

  // The prefix must cover the relative instruction the entry resolves.
  const size_t minimum =
      signature.target == SignatureTarget::kMatch
          ? 6
          : signature.delta + (signature.target == SignatureTarget::kCall ? 5 : 7);
  const auto window = old.image_bytes().subspan(
      hits.front(), std::min(minimum + kSearchSlack, old.image_bytes().size() - hits.front()));
  // Wildcards already in the recorded pattern stay wildcards.
  std::vector<bool> wildcard(window.size(), false);
  std::copy_n(parsed->wildcard.begin(), std::min(parsed->wildcard.size(), wildcard.size()),
              wildcard.begin());
  ZydisDecoder decoder;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  size_t end = 0;
  while (end < window.size()) {
    ZydisDecoderContext context;
    ZydisDecodedInstruction instruction;
    if (ZYAN_FAILED(ZydisDecoderDecodeInstruction(&decoder, &context, window.data() + end,
                                                  window.size() - end, &instruction))) {
      break;
    }
    if (instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) {
      const auto& raw = instruction.raw;
      if (raw.disp.size != 0) {
        std::fill_n(wildcard.begin() + end + raw.disp.offset, raw.disp.size / 8, true);
      }
      for (const auto& imm : raw.imm) {
        if (imm.is_relative && imm.size != 0) {
          std::fill_n(wildcard.begin() + end + imm.offset, imm.size / 8, true);
        }
      }
    }
    end += instruction.length;
    if (end < minimum) continue;

    const auto candidate = Pattern(window.first(end), wildcard);
    if (Hits(current, candidate) != 1) continue;
    GameSignature replacement = signature;
    replacement.pattern = candidate;
    if (modlock::gameinterop::ResolveSignature(current, replacement)) return candidate;
  }
  return std::nullopt;
}

struct Install {
  fs::path root;
  std::optional<PeImage> modules[3];
};

// ModuleIndex maps a GameModule bit to its slot in Install::modules.
size_t ModuleIndex(GameModule module) {
  switch (module) {
    case GameModule::kServer:
      return 0;
    case GameModule::kClient:
      return 1;
    case GameModule::kEngine:
      return 2;
  }
  return 0;
}

constexpr GameModule kModules[] = {GameModule::kServer, GameModule::kClient, GameModule::kEngine};

// LoadInstall maps every module the signature table names.
std::expected<Install, std::string> LoadInstall(const fs::path& root) {
  Install install{.root = root};
  for (const auto module : kModules) {
    const bool named = std::ranges::any_of(modlock::gameinterop::GameSignatures(),
                                           [module](const auto& s) { return s.In(module); });
    if (!named) continue;
    auto image = PeImage::Load(ModulePath(root, module));
    if (!image) return std::unexpected(image.error());
    install.modules[ModuleIndex(module)] = std::move(*image);
  }
  return install;
}

}  // namespace

int main(int argc, char** argv) {
  std::optional<fs::path> game;
  std::optional<fs::path> baseline_root;
  std::optional<fs::path> signatures;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--baseline" && i + 1 < argc) {
      baseline_root = argv[++i];
    } else if (arg == "--signatures" && i + 1 < argc) {
      signatures = argv[++i];
    } else if (!arg.starts_with("--") && !game) {
      game = arg;
    } else {
      game.reset();
      break;
    }
  }
  if (!game) {
    std::fprintf(stderr,
                 "usage: modlock-sigcheck <deadlock-dir> [--baseline <old-deadlock-dir>] "
                 "[--signatures <game_signatures.txtpb>]\n");
    return 2;
  }

  // A skipped entry fails the check like an entry that does not resolve.
  size_t failed = 0;
  if (signatures) {
    const auto load = modlock::gameinterop::LoadGameSignatures(*signatures);
    if (!load) {
      std::fprintf(stderr, "%s\n", load.error().c_str());
      return 2;
    }
    for (const auto& skipped : load->skipped) std::printf("FAIL  %s\n", skipped.c_str());
    failed = load->skipped.size();
  }

  auto current = LoadInstall(*game);
  if (!current) {
    std::fprintf(stderr, "%s\n", current.error().c_str());
    return 2;
  }
  std::optional<Install> baseline;
  if (baseline_root) {
    auto loaded = LoadInstall(*baseline_root);
    if (!loaded) {
      std::fprintf(stderr, "baseline: %s\n", loaded.error().c_str());
      return 2;
    }
    baseline = std::move(*loaded);
    std::printf("baseline build %s\n", BuildVersion(*baseline_root).c_str());
  }
  std::printf("build %s\n", BuildVersion(*game).c_str());

  size_t checked = failed;
  for (const auto& signature : modlock::gameinterop::GameSignatures()) {
    for (const auto module : kModules) {
      if (!signature.In(module)) continue;
      ++checked;
      const auto& image = *current->modules[ModuleIndex(module)];
      const auto file = std::string(modlock::gameinterop::GameModuleFile(module));
      const auto id = std::string(signature.id);
      auto resolved = modlock::gameinterop::ResolveSignature(image, signature);
      if (resolved) {
        std::printf("ok    %-12s %-44s rva=0x%zx\n", file.c_str(), id.c_str(),
                    static_cast<size_t>(reinterpret_cast<std::uintptr_t>(*resolved)));
        continue;
      }
      ++failed;
      std::printf("FAIL  %-12s %-44s hits=%zu %s\n", file.c_str(), id.c_str(),
                  Hits(image, signature.pattern), resolved.error().c_str());
      if (!baseline) continue;
      const auto& old = *baseline->modules[ModuleIndex(module)];
      if (auto suggestion = Suggest(signature, old, image)) {
        std::printf("      suggest \"%s\"\n", suggestion->c_str());
      } else {
        std::printf("      no unique candidate from the baseline match; re-derive by hand\n");
      }
    }
  }
  std::printf("%zu of %zu signatures resolved\n", checked - failed, checked);
  return failed == 0 ? 0 : 1;
}
