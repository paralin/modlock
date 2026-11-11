#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "modlock/wasm_host.h"

namespace modlock::host_app {

// ProvideFixtures provides every service the Fixtures message in data names,
// answering each mod's call to a method with that mod's fixture reply. A
// method or a mod the fixtures lack is refused, naming the fixture file. It
// returns one line per service and mod for the host's log, or an error for a
// message that does not parse.
[[nodiscard]] std::expected<std::string, std::string> ProvideFixtures(WasmHost& host,
                                                                      std::string_view data);

}  // namespace modlock::host_app
