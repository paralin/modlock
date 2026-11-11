#pragma once

#include <expected>
#include <functional>
#include <string>
#include <string_view>

#include "modlock/wasm_host.h"

namespace modlock::host_app {

// Forward hands one mod's started call of a service to whoever answers it in
// a later frame, such as the controller.
using Forward = std::function<void(std::string_view mod, std::string_view service,
                                   std::string_view method, std::string_view payload,
                                   WasmReply reply)>;

// ProvideFixtures provides every service the Fixtures message in data names,
// answering each mod's call to a method with that mod's fixture reply. A
// method or a mod the fixtures lack is refused, naming the fixture file. A
// service the message forwards for a mod takes that mod's started calls and
// hands them to forward. It returns one line per service and mod for the
// host's log, or an error for a message that does not parse or that forwards
// with no forward.
[[nodiscard]] std::expected<std::string, std::string> ProvideFixtures(WasmHost& host,
                                                                      std::string_view data,
                                                                      Forward forward = {});

}  // namespace modlock::host_app
