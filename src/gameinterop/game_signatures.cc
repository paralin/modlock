// The recorded signatures and virtual slots come from the game data file,
// data/game_signatures.txtpb. The build embeds a copy so the host and the
// tests start with every entry; a release ships the file beside modlock-host,
// which loads it at start.

#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/text_format.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>

#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/signature.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "proto/modlock/game_signatures.pb.h"

namespace modlock::gameinterop {

// kBuiltInGameSignatures is the game data file as of the build.
extern const std::string_view kBuiltInGameSignatures;

namespace {

// Table is one parsed game data file and the entries that view into it.
struct Table {
  gamedata::GameSignatureFile file;
  std::vector<GameSignature> entries;
  std::vector<VirtualSlot> slots;
};

// FirstError keeps the text format parser's first error.
class FirstError final : public google::protobuf::io::ErrorCollector {
 public:
  void RecordError(int line, google::protobuf::io::ColumnNumber column,
                   absl::string_view message) override {
    if (!text.empty()) {
      return;
    }
    text = "line " + std::to_string(line + 1) + ", column " + std::to_string(column + 1) + ": " +
           std::string(message);
  }

  std::string text;
};

// Module returns the GameModule bit for module, or zero for an unknown one.
uint8_t Module(int module) {
  switch (module) {
    case gamedata::GAME_MODULE_SERVER:
      return static_cast<uint8_t>(GameModule::kServer);
    case gamedata::GAME_MODULE_CLIENT:
      return static_cast<uint8_t>(GameModule::kClient);
    case gamedata::GAME_MODULE_ENGINE:
      return static_cast<uint8_t>(GameModule::kEngine);
    default:
      return 0;
  }
}

// Entry checks one file entry and views it as a GameSignature.
std::expected<GameSignature, std::string> Entry(const gamedata::GameSignature& entry) {
  // Every entry needs an id.
  if (entry.id().empty()) {
    return std::unexpected("an entry has no id");
  }
  const std::string named = "signature '" + entry.id() + "'";

  // Collect the modules the signature searches.
  GameSignature signature{
      .id = entry.id(), .modules = 0, .pattern = entry.pattern(), .shape = entry.shape()};
  for (const int module : entry.modules()) {
    const uint8_t bit = Module(module);
    if (bit == 0) {
      return std::unexpected(named + " names an unknown module");
    }
    signature.modules |= bit;
  }
  if (signature.modules == 0) {
    return std::unexpected(named + " names no module");
  }

  // Record what a match points at.
  switch (entry.target_case()) {
    case gamedata::GameSignature::kCallAt:
      signature.target = SignatureTarget::kCall;
      signature.delta = entry.call_at();
      break;
    case gamedata::GameSignature::kRipRelativeAt:
      signature.target = SignatureTarget::kRipRelative;
      signature.delta = entry.rip_relative_at();
      break;
    case gamedata::GameSignature::TARGET_NOT_SET:
      break;
  }

  // The pattern must parse.
  if (auto parsed = ParseSignature(entry.id(), entry.pattern()); !parsed) {
    return std::unexpected(parsed.error());
  }
  return signature;
}

// Slot checks one file entry and views it as a VirtualSlot.
std::expected<VirtualSlot, std::string> Slot(const gamedata::VirtualSlot& entry) {
  // A slot needs an id, a module, a class and a valid pattern.
  if (entry.id().empty()) {
    return std::unexpected("a slot has no id");
  }
  const std::string named = "slot '" + entry.id() + "'";
  if (entry.module().empty() || entry.class_name().empty()) {
    return std::unexpected(named + " names no module or class");
  }
  if (auto parsed = ParseSignature(entry.id(), entry.pattern()); !parsed) {
    return std::unexpected(parsed.error());
  }
  return VirtualSlot{.id = entry.id(),
                     .module = entry.module(),
                     .class_name = entry.class_name(),
                     .index = entry.index(),
                     .pattern = entry.pattern(),
                     .shape = entry.shape()};
}

// Parse reads a game data file, skipping the entries Entry and Slot reject and
// every repeat of an id.
std::expected<std::unique_ptr<const Table>, std::string> Parse(std::string_view text,
                                                               GameSignatureLoad& load) {
  // Parse the text format, keeping the first error.
  auto table = std::make_unique<Table>();
  FirstError error;
  google::protobuf::TextFormat::Parser parser;
  parser.RecordErrorsTo(&error);
  if (!parser.ParseFromString(text, &table->file)) {
    return std::unexpected(error.text);
  }

  // Record each valid signature once, skipping the rest.
  for (const auto& entry : table->file.signatures()) {
    auto signature = Entry(entry);
    if (!signature) {
      load.skipped.push_back(std::move(signature.error()));
      continue;
    }
    if (std::ranges::find(table->entries, signature->id, &GameSignature::id) !=
        table->entries.end()) {
      load.skipped.push_back("signature '" + entry.id() + "' is recorded twice");
      continue;
    }
    table->entries.push_back(*signature);
  }
  std::ranges::sort(table->entries, {}, &GameSignature::id);

  // Record each valid slot once, skipping the rest.
  for (const auto& entry : table->file.slots()) {
    auto slot = Slot(entry);
    if (!slot) {
      load.skipped.push_back(std::move(slot.error()));
      continue;
    }
    if (std::ranges::find(table->slots, slot->id, &VirtualSlot::id) != table->slots.end()) {
      load.skipped.push_back("slot '" + entry.id() + "' is recorded twice");
      continue;
    }
    table->slots.push_back(*slot);
  }
  std::ranges::sort(table->slots, {}, &VirtualSlot::id);

  // Report the counts.
  load.loaded = table->entries.size();
  load.slots = table->slots.size();
  return table;
}

// Recorded holds the current table, the built-in copy until a load replaces it.
std::unique_ptr<const Table>& Recorded() {
  static std::unique_ptr<const Table> table = [] {
    GameSignatureLoad load;
    auto parsed = Parse(kBuiltInGameSignatures, load);
    return parsed ? std::move(*parsed) : std::make_unique<const Table>();
  }();
  return table;
}

}  // namespace

std::span<const GameSignature> GameSignatures() {
  return Recorded()->entries;
}

std::span<const VirtualSlot> GameVirtualSlots() {
  return Recorded()->slots;
}

const VirtualSlot* FindVirtualSlot(std::string_view id) {
  const auto slots = GameVirtualSlots();
  const auto found = std::ranges::find(slots, id, &VirtualSlot::id);
  return found == slots.end() ? nullptr : &*found;
}

std::expected<GameSignatureLoad, std::string> LoadGameSignatures(
    const std::filesystem::path& path) {
  // Read the whole file.
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::unexpected("game signatures: cannot read " + path.string());
  }
  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

  // Parse it and replace the recorded tables.
  GameSignatureLoad load;
  auto table = Parse(text, load);
  if (!table) {
    return std::unexpected("game signatures: " + path.string() + ": " + table.error());
  }
  Recorded() = std::move(*table);
  return load;
}

}  // namespace modlock::gameinterop
