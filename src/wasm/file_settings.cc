#include "modlock/wasm_settings.h"

#include <google/protobuf/json/json.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>

namespace modlock {

FileSettings::FileSettings(std::filesystem::path path) : path_(std::move(path)) {
  // Start empty without a file, or with one that does not read.
  if (path_.empty()) return;
  std::ifstream file(path_, std::ios::binary);
  if (!file) return;
  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  google::protobuf::json::ParseOptions options;
  options.ignore_unknown_fields = true;
  if (auto parsed = google::protobuf::json::JsonStringToMessage(text, &stored_, options);
      !parsed.ok()) {
    std::cerr << "settings: " << path_.string() << ": " << parsed.message()
              << "; starting without them\n";
    stored_.Clear();
  }
}

std::optional<std::string> FileSettings::Value(std::string_view mod, uint64_t steam_id,
                                               std::string_view key) {
  for (const auto& stored : stored_.values()) {
    if (stored.mod() == mod && stored.steam_id() == steam_id && stored.key() == key) {
      return stored.value();
    }
  }
  return std::nullopt;
}

void FileSettings::Store(std::string_view mod, uint64_t steam_id, std::string_view key,
                         std::string_view value) {
  // Replace the player's earlier value, or add the first.
  wasm::StoredSetting* found = nullptr;
  for (auto& stored : *stored_.mutable_values()) {
    if (stored.mod() == mod && stored.steam_id() == steam_id && stored.key() == key) {
      found = &stored;
      break;
    }
  }
  if (!found) {
    found = stored_.add_values();
    found->set_mod(mod);
    found->set_steam_id(steam_id);
    found->set_key(key);
  }
  found->set_value(value);

  // Rewrite the file whole through a staged copy.
  if (path_.empty()) return;
  std::string text;
  google::protobuf::json::PrintOptions options;
  options.add_whitespace = true;
  if (auto printed = google::protobuf::json::MessageToJsonString(stored_, &text, options);
      !printed.ok()) {
    std::cerr << "settings: " << printed.message() << '\n';
    return;
  }
  std::error_code error;
  std::filesystem::create_directories(path_.parent_path(), error);
  auto staged = path_;
  staged += ".tmp";
  {
    std::ofstream file(staged, std::ios::binary | std::ios::trunc);
    file << text;
    if (!file) {
      std::cerr << "settings: cannot write " << staged.string() << '\n';
      return;
    }
  }
  std::filesystem::rename(staged, path_, error);
  if (error) std::cerr << "settings: cannot write " << path_.string() << ": " << error.message() << '\n';
}

}  // namespace modlock
