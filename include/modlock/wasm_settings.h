#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "modlock/export.h"
#include "proto/modlock/wasm.pb.h"

namespace modlock {

// WasmSettings keeps the values players chose for mods' settings, by mod,
// player and setting key. A WasmHost checks each value against the mod's
// manifest before storing it and resolves a missing or stale one to the
// setting's default. Its methods run on the engine thread, so they answer
// from memory and save without waiting.
class MODLOCK_API WasmSettings {
 public:
  virtual ~WasmSettings() = default;

  // Value returns the value the player chose for the mod's setting, or
  // nothing.
  virtual std::optional<std::string> Value(std::string_view mod, uint64_t steam_id,
                                           std::string_view key) = 0;

  // Store keeps the player's new value of the mod's setting.
  virtual void Store(std::string_view mod, uint64_t steam_id, std::string_view key,
                     std::string_view value) = 0;
};

// FileSettings keeps settings in memory and, given a path, in a JSON file of
// wasm.StoredSettings that it reads when constructed and rewrites on each
// change. A file that cannot be read starts empty, and one that cannot be
// written keeps the values in memory, each with a log line.
class MODLOCK_API FileSettings final : public WasmSettings {
 public:
  explicit FileSettings(std::filesystem::path path = {});

  std::optional<std::string> Value(std::string_view mod, uint64_t steam_id,
                                   std::string_view key) override;
  void Store(std::string_view mod, uint64_t steam_id, std::string_view key,
             std::string_view value) override;

 private:
  std::filesystem::path path_;
  wasm::StoredSettings stored_;
};

}  // namespace modlock
