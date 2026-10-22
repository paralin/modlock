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
// It then checks every virtual slot in the game data: the class's dispatch
// table, found through its RTTI name, must hold exactly one function that
// matches the slot's pattern. Modlock finds that function wherever it sits, so
// a slot that moved still holds; its line prints the new index to record.
//
//   modlock-sigcheck <deadlock-dir> [--baseline <old-deadlock-dir>]
//                    [--signatures <game_signatures.txtpb>]
//
// Exit status is zero only when every signature resolves and every slot holds.
// With --signatures, the slots also come from that file.
#include <Zydis.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/signature.h"
#include "modlock/gameinterop/virtual_slots.h"

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
// copied to its virtual address, so scan offsets are RVAs. base() is zero, and
// pointer_base() is the preferred base the file's absolute pointers count from.
class PeImage final : public ModuleImage {
 public:
  static std::expected<PeImage, std::string> Load(const fs::path& path) {
    // Read the whole file.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return std::unexpected("cannot open " + path.string());
    }
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(in), {}};

    // Read little-endian words, or nothing past the end.
    auto read32 = [&file](size_t at) -> std::optional<uint32_t> {
      if (at + 4 > file.size()) {
        return std::nullopt;
      }
      uint32_t value = 0;
      std::memcpy(&value, file.data() + at, 4);
      return value;
    };
    auto read16 = [&file](size_t at) -> std::optional<uint16_t> {
      if (at + 2 > file.size()) {
        return std::nullopt;
      }
      uint16_t value = 0;
      std::memcpy(&value, file.data() + at, 2);
      return value;
    };

    // Find the PE header.
    const auto pe = read32(0x3C);
    if (!pe || read32(*pe) != 0x00004550u) {
      return std::unexpected(path.string() + " is not a PE");
    }

    // Read the COFF and optional header fields.
    const size_t coff = *pe + 4;
    const auto section_count = read16(coff + 2);
    const auto optional_size = read16(coff + 16);
    const size_t optional = coff + 20;
    const auto image_base_low = read32(optional + 24);
    const auto image_base_high = read32(optional + 28);
    const auto image_size = read32(optional + 56);
    const auto header_size = read32(optional + 60);

    // A field past the end of the file is a truncated header.
    if (!section_count || !optional_size || !image_base_low || !image_base_high || !image_size ||
        !header_size) {
      return std::unexpected(path.string() + " has a truncated PE header");
    }

    // Map the headers, then each section at its virtual address.
    PeImage image;
    image.image_base_ = uint64_t{*image_base_high} << 32 | *image_base_low;
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
      if (*raw_offset >= file.size() || *virtual_address >= image.bytes_.size()) {
        continue;
      }
      const size_t count = std::min<size_t>(
          {*raw_size, file.size() - *raw_offset, image.bytes_.size() - *virtual_address});
      std::memcpy(image.bytes_.data() + *virtual_address, file.data() + *raw_offset, count);
    }
    return image;
  }

  std::uintptr_t base() const override { return 0; }
  std::uintptr_t pointer_base() const override { return image_base_; }
  std::span<const uint8_t> image_bytes() const override { return bytes_; }

 private:
  std::uintptr_t image_base_ = 0;
  std::vector<uint8_t> bytes_;
};

// ModulePath returns where the module named file lives under a Deadlock
// install: the game's own modules beside its content, the rest in the engine.
fs::path ModulePath(const fs::path& game, std::string_view file) {
  if (file == "server.dll" || file == "client.dll") {
    return game / "game" / "citadel" / "bin" / "win64" / file;
  }
  return game / "game" / "bin" / "win64" / file;
}

// BuildVersion reads the ClientVersion line of the install's steam.inf.
std::string BuildVersion(const fs::path& game) {
  std::ifstream in(game / "game" / "citadel" / "steam.inf");
  std::string line;
  while (std::getline(in, line)) {
    if (line.starts_with("ClientVersion=")) {
      return line.substr(14);
    }
  }
  return "unknown";
}

// Hits counts the pattern's matches in image.
size_t Hits(const ModuleImage& image, std::string_view pattern) {
  auto parsed = modlock::gameinterop::ParseSignature("hits", pattern);
  if (!parsed || parsed->bytes.empty()) {
    return 0;
  }
  return modlock::gameinterop::SignatureScan(image.image_bytes(), *parsed).size();
}

// Pattern renders bytes in the signature syntax, "??" for wildcards.
std::string Pattern(std::span<const uint8_t> bytes, const std::vector<bool>& wildcard) {
  std::string out;
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (!out.empty()) {
      out += ' ';
    }
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
  // The entry must match exactly once in the old build.
  auto parsed = modlock::gameinterop::ParseSignature(std::string(signature.id), signature.pattern);
  if (!parsed) {
    return std::nullopt;
  }
  const auto hits = modlock::gameinterop::SignatureScan(old.image_bytes(), *parsed);
  if (hits.size() != 1) {
    return std::nullopt;
  }

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

  // Grow the prefix one instruction at a time until it matches once in the new build.
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
    if (end < minimum) {
      continue;
    }

    const auto candidate = Pattern(window.first(end), wildcard);
    if (Hits(current, candidate) != 1) {
      continue;
    }
    GameSignature replacement = signature;
    replacement.pattern = candidate;
    if (modlock::gameinterop::ResolveSignature(current, replacement)) {
      return candidate;
    }
  }
  return std::nullopt;
}

// Install maps each module file a check reads to its image.
struct Install {
  fs::path root;
  std::map<std::string, PeImage> modules;
};

constexpr GameModule kModules[] = {GameModule::kServer, GameModule::kClient, GameModule::kEngine};

// LoadInstall maps every module the signature table names and, with slots,
// every module a virtual slot names.
std::expected<Install, std::string> LoadInstall(const fs::path& root, bool slots) {
  // List the modules that signatures and slots name.
  std::vector<std::string> files;
  for (const auto module : kModules) {
    if (std::ranges::any_of(modlock::gameinterop::GameSignatures(),
                            [module](const auto& s) { return s.In(module); })) {
      files.emplace_back(modlock::gameinterop::GameModuleFile(module));
    }
  }
  if (slots) {
    for (const auto& slot : modlock::gameinterop::GameVirtualSlots()) {
      files.emplace_back(slot.module);
    }
  }

  // Load each module once.
  Install install{.root = root};
  for (const auto& file : files) {
    if (install.modules.contains(file)) {
      continue;
    }
    auto image = PeImage::Load(ModulePath(root, file));
    if (!image) {
      return std::unexpected(image.error());
    }
    install.modules.emplace(file, std::move(*image));
  }
  return install;
}

// CheckSlot reports whether exactly one function of slot's table matches its
// pattern in image.
bool CheckSlot(const ModuleImage& image, const modlock::gameinterop::VirtualSlot& slot) {
  // Find the slot's vtable and the functions that match.
  const auto id = std::string(slot.id);
  const auto module = std::string(slot.module);
  const auto table = modlock::gameinterop::FindVtable(image, slot.class_name);
  auto matches =
      table ? modlock::gameinterop::MatchingSlots(image, *table, slot)
            : std::expected<std::vector<size_t>, std::string>(std::unexpected(table.error()));
  if (matches && matches->size() == 1) {
    const size_t index = matches->front();
    if (index == slot.index) {
      std::printf("ok    %-17s %-40s [%zu]\n", module.c_str(), id.c_str(), index);
    } else {
      std::printf("ok    %-17s %-40s [%zu] moved from %zu; record the new index\n", module.c_str(),
                  id.c_str(), index, slot.index);
    }
    return true;
  }

  // Explain why the slot does not hold.
  std::string reason;
  if (!matches) {
    reason = matches.error();
  } else if (matches->empty()) {
    reason = "no function of the table matches; re-derive the pattern";
  } else {
    reason = "matches slots";
    for (const size_t index : *matches) {
      reason += " " + std::to_string(index);
    }
    reason += "; lengthen the pattern";
  }
  std::printf("FAIL  %-17s %-40s %s\n      %s\n", module.c_str(), id.c_str(), reason.c_str(),
              std::string(slot.shape).c_str());
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  // Read the game directory and the options.
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
    for (const auto& skipped : load->skipped) {
      std::printf("FAIL  %s\n", skipped.c_str());
    }
    failed = load->skipped.size();
  }

  // Load the current install and the optional baseline.
  auto current = LoadInstall(*game, true);
  if (!current) {
    std::fprintf(stderr, "%s\n", current.error().c_str());
    return 2;
  }
  std::optional<Install> baseline;
  if (baseline_root) {
    auto loaded = LoadInstall(*baseline_root, false);
    if (!loaded) {
      std::fprintf(stderr, "baseline: %s\n", loaded.error().c_str());
      return 2;
    }
    baseline = std::move(*loaded);
    std::printf("baseline build %s\n", BuildVersion(*baseline_root).c_str());
  }
  std::printf("build %s\n", BuildVersion(*game).c_str());

  // Resolve each signature, suggesting a fix from the baseline.
  size_t checked = failed;
  for (const auto& signature : modlock::gameinterop::GameSignatures()) {
    for (const auto module : kModules) {
      if (!signature.In(module)) {
        continue;
      }
      ++checked;
      const auto file = std::string(modlock::gameinterop::GameModuleFile(module));
      const auto& image = current->modules.at(file);
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
      if (!baseline) {
        continue;
      }
      const auto& old = baseline->modules.at(file);
      if (auto suggestion = Suggest(signature, old, image)) {
        std::printf("      suggest \"%s\"\n", suggestion->c_str());
      } else {
        std::printf("      no unique candidate from the baseline match; re-derive by hand\n");
      }
    }
  }
  std::printf("%zu of %zu signatures resolved\n", checked - failed, checked);

  // Check each virtual slot.
  size_t broken = 0;
  const auto slots = modlock::gameinterop::GameVirtualSlots();
  for (const auto& slot : slots) {
    if (!CheckSlot(current->modules.at(std::string(slot.module)), slot)) {
      ++broken;
    }
  }
  std::printf("%zu of %zu virtual slots hold\n", slots.size() - broken, slots.size());
  return failed == 0 && broken == 0 ? 0 : 1;
}
