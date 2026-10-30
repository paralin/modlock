//go:build deps_only

#include "wasm/files.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace modlock::wasm {

bool FileName(std::string_view name) {
  // Accept "/"-separated segments without "." or "..", empty segments or
  // characters another platform reads as a separator.
  if (name.empty()) {
    return false;
  }
  for (size_t start = 0; start <= name.size();) {
    const auto end = std::min(name.find('/', start), name.size());
    const auto segment = name.substr(start, end - start);
    if (segment.empty() || segment == "." || segment == "..") {
      return false;
    }
    for (const char c : segment) {
      if (c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) {
        return false;
      }
    }
    start = end + 1;
  }
  return true;
}

namespace {

// Resolve returns the path of the file name below dir, or an error for a
// name that is not a plain relative path or that leaves dir, such as
// through a symbolic link.
std::expected<std::filesystem::path, std::string> Resolve(const std::filesystem::path& dir,
                                                          std::string_view name) {
  const auto refuse = [&] {
    return std::unexpected("not a file name: " + std::string(name));
  };
  if (!FileName(name)) {
    return refuse();
  }
  const std::filesystem::path relative(std::u8string(name.begin(), name.end()));

  // Keep the resolved path under the resolved directory.
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(dir, error);
  if (error) {
    return std::unexpected(dir.string() + ": " + error.message());
  }
  auto path = std::filesystem::weakly_canonical(root / relative, error);
  if (error) {
    return std::unexpected(std::string(name) + ": " + error.message());
  }
  const auto [stop, rest] = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
  if (stop != root.end() || rest == path.end()) {
    return refuse();
  }
  return path;
}

// ReadText returns the text of path, or nullopt when there is no such file.
std::expected<std::optional<std::string>, std::string> ReadText(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    return std::nullopt;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::unexpected("cannot read " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Used returns the bytes the files below dir hold, apart from skip.
uintmax_t Used(const std::filesystem::path& dir, const std::filesystem::path& skip) {
  uintmax_t used = 0;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(dir, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    if (it->is_regular_file(error) && it->path() != skip) {
      used += it->file_size(error);
    }
  }
  return used;
}

}  // namespace

std::expected<std::optional<std::string>, std::string> ModFiles::Read(std::string_view name) const {
  // Look in the written files first, then in the build's.
  for (const auto* dir : {&written, &data}) {
    if (dir->empty()) {
      continue;
    }
    auto path = Resolve(*dir, name);
    if (!path) {
      return std::unexpected(path.error());
    }
    auto text = ReadText(*path);
    if (!text || *text) {
      return text;
    }
  }
  return std::nullopt;
}

std::expected<void, std::string> ModFiles::Write(std::string_view name,
                                                 std::string_view text) const {
  // Refuse a write the host keeps nowhere, or that the quota cannot hold.
  if (written.empty()) {
    return std::unexpected("this host keeps no mod files");
  }
  std::error_code error;
  std::filesystem::create_directories(written, error);
  if (error) {
    return std::unexpected(written.string() + ": " + error.message());
  }
  auto path = Resolve(written, name);
  if (!path) {
    return std::unexpected(path.error());
  }
  if (Used(written, *path) + text.size() > kFilesQuota) {
    return std::unexpected("the mod's files would pass their 4 MiB quota");
  }

  // Write a temporary file beside the target and rename it into place.
  std::filesystem::create_directories(path->parent_path(), error);
  if (error) {
    return std::unexpected(path->parent_path().string() + ": " + error.message());
  }
  auto temporary = *path;
  temporary += ".writing";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.flush()) {
      file.close();
      std::filesystem::remove(temporary, error);
      return std::unexpected("cannot write " + path->string());
    }
  }
  std::filesystem::rename(temporary, *path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return std::unexpected(path->string() + ": " + error.message());
  }
  return {};
}

}  // namespace modlock::wasm
