//go:build deps_only

#include "wasm/build.h"

#include <google/protobuf/json/json.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

namespace modlock::wasm {

namespace {

// ReadFile returns the bytes of path.
std::expected<std::vector<uint8_t>, std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::unexpected("cannot read " + path.string());
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
}

// InterpreterFile names the interpreter module that runs an interpreted
// runtime, or is empty for a compiled one.
std::string_view InterpreterFile(wasm::Manifest::Runtime runtime) {
  switch (runtime) {
    case wasm::Manifest::RUNTIME_QUICKJS:
      return "quickjs.wasm";
    case wasm::Manifest::RUNTIME_LUAU:
      return "luau.wasm";
    case wasm::Manifest::RUNTIME_PYTHON:
      return "python.wasm";
    default:
      return {};
  }
}

// IsSlug reports whether name is a slug, as check.Slug holds one.
bool IsSlug(std::string_view name) {
  return !name.empty() && !name.starts_with('-') && !name.ends_with('-') && !name.contains("--") &&
         std::ranges::all_of(name, [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
         });
}

}  // namespace

// ReadBuild reads a built mod directory through its mod.json, or a bare
// .wasm file. An interpreted mod's module comes from interpreters.
std::expected<Build, std::string> ReadBuild(const std::filesystem::path& path,
                                            const std::filesystem::path& interpreters) {
  // A bare module is its own build.
  if (!std::filesystem::is_directory(path)) {
    auto module = ReadFile(path);
    if (!module) {
      return std::unexpected(module.error());
    }
    if (!IsSlug(path.stem().string())) {
      return std::unexpected(path.string() + ": invalid slug");
    }
    return Build{.name = path.stem().string(), .module = std::move(*module)};
  }

  // Parse the manifest, tolerating fields a newer command line writes.
  const auto manifest_path = path / "mod.json";
  auto text = ReadFile(manifest_path);
  if (!text) {
    return std::unexpected(text.error());
  }
  wasm::Manifest manifest;
  google::protobuf::json::ParseOptions options;
  options.ignore_unknown_fields = true;
  const auto parsed = google::protobuf::json::JsonStringToMessage(
      std::string_view(reinterpret_cast<const char*>(text->data()), text->size()), &manifest,
      options);
  if (!parsed.ok()) {
    return std::unexpected(manifest_path.string() + ": " + std::string(parsed.message()));
  }

  // Read the entry the manifest names, the interpreter that runs it and the
  // collision its movement model needs.
  if (manifest.slug().empty()) {
    return std::unexpected(manifest_path.string() + ": no slug");
  }
  if (!IsSlug(manifest.slug())) {
    return std::unexpected(manifest_path.string() + ": invalid slug");
  }
  auto entry = ReadFile(path / manifest.entry());
  if (!entry) {
    return std::unexpected(entry.error());
  }
  Build build{.name = manifest.slug(), .data = path / "data"};
  if (manifest.movement().model() == wasm::Movement::MODEL_QUAKEWORLD) {
    auto collision = ReadFile(path / "maps" / (manifest.map() + ".bsp"));
    if (!collision) {
      return std::unexpected(collision.error());
    }
    build.collision = std::move(*collision);
  }
  if (manifest.runtime() == wasm::Manifest::RUNTIME_WASM) {
    build.module = std::move(*entry);
  } else {
    // Other runtimes run their source in the host's interpreter.
    const auto interpreter = InterpreterFile(manifest.runtime());
    if (interpreter.empty()) {
      return std::unexpected(manifest_path.string() + ": this host does not know the runtime");
    }
    auto module = ReadFile(interpreters / interpreter);
    if (!module) {
      return std::unexpected(module.error());
    }
    build.module = std::move(*module);
    build.source = std::move(*entry);
  }
  build.manifest = std::move(manifest);
  return build;
}

}  // namespace modlock::wasm
