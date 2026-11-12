#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "src/wasm/manifest.pb.h"

namespace modlock::wasm {

// kAddonToken stands for the mod's content addon in a text value or path.
inline constexpr std::string_view kAddonToken = "{addon}";

// ContentAddon returns the content addon a manifest mounts: its own
// content_addon, or slug-version when it names none.
inline std::string ContentAddon(const Manifest& manifest) {
  return manifest.content_addon().empty() ? manifest.slug() + "-" + manifest.version()
                                          : manifest.content_addon();
}

// WithAddon returns text with each {addon} replaced by addon, or text as it is
// when addon is empty.
inline std::string WithAddon(std::string text, std::string_view addon) {
  if (addon.empty()) {
    return text;
  }
  for (auto at = text.find(kAddonToken); at != std::string::npos;
       at = text.find(kAddonToken, at + addon.size())) {
    text.replace(at, kAddonToken.size(), addon);
  }
  return text;
}

// Interned returns a copy of text that lives for the life of the process, one
// copy per distinct text. A field the game never frees, such as a symbol or a
// borrowed buffer string, may point to it past the mod that wrote it. It is
// not thread safe; callers use it on the engine thread.
inline const char* Interned(std::string text) {
  static auto* texts = new std::unordered_set<std::string>();
  return texts->insert(std::move(text)).first->c_str();
}

}  // namespace modlock::wasm
