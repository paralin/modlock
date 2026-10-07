#pragma once

#include <windows.h>

#include <expected>
#include <string>

namespace modlock::host_app {

// ListenEngineLog writes every engine log message to stdout for the process
// lifetime, which a dedicated server needs once it runs without the engine's
// text console. engine2 replaces the logging state with an empty one during
// startup and again around some operations, so ListenEngineLog patches
// engine2's tier0 imports to rejoin each new state as well as joining the
// current one. Call it once, after engine2 is mapped and before it starts.
[[nodiscard]] std::expected<void, std::string> ListenEngineLog(HMODULE engine2);

}  // namespace modlock::host_app
