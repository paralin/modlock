#pragma once

#include <windows.h>

#include <expected>
#include <string>
#include <string_view>

namespace modlock::host_app {

// PatchImport points module's import address table entry for the function
// that dll exports as name, or as ordinal when module imports it by ordinal,
// at replacement. It returns the function the entry held before. Pass ordinal
// 0 to match by name only. The patch lasts for the module's lifetime.
[[nodiscard]] std::expected<void*, std::string> PatchImport(HMODULE module, std::string_view dll,
                                                            std::string_view name, WORD ordinal,
                                                            void* replacement);

}  // namespace modlock::host_app
