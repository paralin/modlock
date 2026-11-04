#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "src/wasm/manifest.pb.h"
#include "wasm/instance.h"

namespace modlock::wasm {

// Build is one built mod read from disk.
struct Build {
  // name identifies the mod among the host's plugins.
  std::string name;
  // module holds the WebAssembly bytes to run.
  std::vector<uint8_t> module;
  // source holds an interpreted mod's entry, which module evaluates at start.
  std::vector<uint8_t> source;
  // manifest is the mod's mod.json; a bare .wasm file has an empty one.
  wasm::Manifest manifest;
  // collision holds the map's Quake collision when the manifest's movement
  // model needs it.
  std::vector<uint8_t> collision;
  // data is the build's data/ directory, the files the mod ships, or empty
  // for a bare .wasm file.
  std::filesystem::path data;
};

// ReadBuild reads a built mod directory through its mod.json, or a bare
// .wasm file. An interpreted mod's module comes from interpreters.
std::expected<Build, std::string> ReadBuild(const std::filesystem::path& path,
                                            const std::filesystem::path& interpreters);

}  // namespace modlock::wasm
