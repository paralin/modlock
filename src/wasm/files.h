#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace modlock::wasm {

// kFilesQuota bounds the bytes one mod's written files hold together.
inline constexpr uintmax_t kFilesQuota = 4u << 20;

// FileName reports whether name is a file name a mod may read or write: a
// relative path of "/"-separated segments, none empty, "." or "..", without
// characters another platform reads as a separator.
bool FileName(std::string_view name);

// ModFiles is one mod's files: those it wrote, kept in written, over those
// its build ships in data. Either directory may be empty, which holds no
// files; an empty written refuses every write.
struct ModFiles {
  std::filesystem::path written;
  std::filesystem::path data;

  // Read returns the text of the file name, from written when it holds one
  // and from data otherwise, or nullopt when neither does.
  std::expected<std::optional<std::string>, std::string> Read(std::string_view name) const;

  // Write replaces the file name in written with text, whole or not at all,
  // while the mod's written files stay within kFilesQuota.
  std::expected<void, std::string> Write(std::string_view name, std::string_view text) const;
};

}  // namespace modlock::wasm
